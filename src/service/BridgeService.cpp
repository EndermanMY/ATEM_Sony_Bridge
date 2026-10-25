#include "BridgeService.h"
#include "Version.h"
#include "common/Validation.h"
#include "common/Logger.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;
static nlohmann::json exactInteger(int64_t value,bool unsigned64=false) {
    if(unsigned64 && value<0)return std::to_string(static_cast<uint64_t>(value));
    if(value>9007199254740991LL || value < -9007199254740991LL) return std::to_string(value);
    return value;
}
struct BusyScope {
    std::atomic<bool>& busy;
    explicit BusyScope(std::atomic<bool>& b):busy(b){busy=true;}
    ~BusyScope(){busy=false;}
};

// TCP 在线探测：非阻塞 connect。
// 连接成功或被拒绝（RST）都说明相机主机在线；超时/不可达才算离线。
// 相机关机后 WiFi TCP 变"半开"，SDK 收不到断开事件，靠此探测兜底。
static bool tcpReachable(const std::string& ip, unsigned short port, int timeoutMs) {
    static const bool wsaReady = [] {
        WSADATA w;
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    if (!wsaReady) return true;   // 探测不可用时按在线处理，避免误断

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return true;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { closesocket(s); return true; }

    bool alive = true;
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            int sel = select(0, nullptr, &wset, nullptr, &tv);
            if (sel <= 0) {
                alive = false;   // 超时：主机无响应
            } else {
                int soErr = 0, len = sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len);
                // 拒绝连接 = 相机在线（遥控端口被现有会话占用很常见）
                alive = (soErr == 0 || soErr == WSAECONNREFUSED);
            }
        } else if (err == WSAECONNREFUSED) {
            alive = true;   // RST 说明主机活着
        } else {
            alive = false;  // 不可达/网络错误
        }
    }
    closesocket(s);
    return alive;
}

BridgeService::BridgeService() {
    m_mapping.setAtemController(&m_atem);
}

// ---------------- 多相机控制器管理 ----------------
CameraController* BridgeService::camera(const std::string& ip) {
    std::lock_guard<std::recursive_mutex> stateLock(m_mutex);
    std::lock_guard<std::recursive_mutex> registryLock(m_registryMutex);
    // 空 ip = 当前选中相机；仍无则取第一台；都没有就建一个占位（ip 为空）
    std::string key = ip;
    if (key.empty()) key = m_activeIp;
    if (key.empty()) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_config.getConfig().cameras.empty()) key = m_config.getConfig().cameras.front().ip;
    }
    if (key.empty()) key = "_default";
    auto it = m_cameras.find(key);
    if (it == m_cameras.end()) {
        it = m_cameras.emplace(key, std::make_unique<CameraController>()).first;
        CameraController* c = it->second.get();
        c->setStatusChangedCallback([this, c](CameraStatus status) { onCameraStatus(c, status); });
    }
    return it->second.get();
}

CameraController* BridgeService::findCamera(const std::string& ip) const {
    std::lock_guard<std::recursive_mutex> registryLock(m_registryMutex);
    auto it = m_cameras.find(ip);
    return it == m_cameras.end() ? nullptr : it->second.get();
}

std::string BridgeService::ipOf(const CameraController* c) const {
    std::lock_guard<std::recursive_mutex> registryLock(m_registryMutex);
    for (const auto& [ip, ptr] : m_cameras) if (ptr.get() == c) return ip;
    return std::string();
}

void BridgeService::setActiveCamera(const std::string& ip) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_activeIp = ip;
    }
    refreshMappingCamera();
    Logger::info("当前选中相机: {}", ip);
}

std::string BridgeService::activeCameraIp() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_activeIp;
}

void BridgeService::refreshMappingCamera() { /* Mapping requires an explicit target. */
}

bool BridgeService::isCameraConnected(const std::string& ip) const {
    CameraController* c = findCamera(ip.empty() ? activeCameraIp() : ip);
    return c && c->isConnected();
}

bool BridgeService::isCameraRecording(const std::string& ip) const {
    CameraController* c = findCamera(ip.empty() ? activeCameraIp() : ip);
    return c && c->isRecording();
}

bool BridgeService::isTallySupported(const std::string& ip) const {
    std::string key = ip.empty() ? activeCameraIp() : ip;
    auto it = m_tallyByIp.find(key);
    return it != m_tallyByIp.end() && it->second;
}

// 相机状态回调（每控制器实例各自注册）
void BridgeService::onCameraStatus(CameraController* cam, CameraStatus status) {
    Logger::info("Camera {} status {}", ipOf(cam), static_cast<int>(status));
}

BridgeService::~BridgeService() {
    shutdown();
}

std::string BridgeService::resourcePath(const std::string& relative) {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    return (fs::path(path).parent_path() / relative).u8string();
}

bool BridgeService::startup() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::string configPath = resourcePath("config/config.json");
    fs::create_directories(fs::u8path(configPath).parent_path());
    if (!fs::exists(fs::u8path(configPath))) {
        auto source = resourcePath("config/config.default.json");
        if (fs::exists(fs::u8path(source))) fs::copy_file(fs::u8path(source), fs::u8path(configPath));
    }
    if (!fs::exists(fs::u8path(resourcePath("config/mapping_rules.json")))) {
        auto source = resourcePath("config/mapping_rules.default.json");
        if (fs::exists(fs::u8path(source))) fs::copy_file(fs::u8path(source), fs::u8path(resourcePath("config/mapping_rules.json")));
    }
    if (!m_config.loadConfig(configPath)) return false;
    if (m_config.migrationNeeded() && !m_config.saveConfig(configPath)) return false;

    // Apply log level from config
    const std::string& lvl = m_config.getConfig().log.level;
    if (lvl == "trace") Logger::setLevel(LogLevel::TRACE);
    else if (lvl == "debug") Logger::setLevel(LogLevel::DEBUG);
    else if (lvl == "info") Logger::setLevel(LogLevel::INFO);
    else if (lvl == "warn") Logger::setLevel(LogLevel::WARN);
    else if (lvl == "error") Logger::setLevel(LogLevel::ERR);

    if (m_config.getConfig().mapping.autoLoadRules) {
        std::string rulesPath = resourcePath("config/" + m_config.getRulesFile());
        m_mapping.loadRulesFromFile(rulesPath);
    }

    wireCallbacks();

    m_started = true;
    Logger::info("Bridge service started (config: {})", configPath);

    if (!CameraController::initializeSDK()) { m_started = false; return false; }
    Logger::info("未发现索尼相机前，状态栏显示搜索中（相机请进入：网络 > 遥控拍摄，停在配对界面）");
    // 后台轮询发现：所有枚举都在工作线程，保证窗口立即可用
    startPolling();
    // NOTIFY 被动发现：路由器组播受限（相机收不到 M-SEARCH）时的兜底方案，
    // 靠相机自身周期广播发现相机 IP
    m_notify.setOnDiscovered([this](const NotifyDiscovery::CameraEndpoint& ep) {
        std::lock_guard<std::mutex> lock(m_discoveredMutex);
        for (const auto& c : m_discovered) {
            if (c.ipAddress == ep.ip) return;   // 已知
        }
        CameraInfo info;
        info.name = "Sony Imaging Device";
        info.model = "Sony 相机";
        info.serialNumber = ep.uuid.empty() ? ep.ip : ep.uuid;
        info.connectionType = "NOTIFY";
        info.ipAddress = ep.ip;
        m_discovered.push_back(info);
    });
    m_notify.start();
    return true;
}

// ---------------- 已绑定相机库 ----------------
nlohmann::json BridgeService::addBoundCamera(const std::string& ip, int modelId, const std::string& name,
                                             const std::string& sshUser, const std::string& sshPass, int cameraId) {
    if (modelId < 0 || modelId >= (int)CameraController::modelNames().size()) throw std::invalid_argument("Invalid model");
    in_addr address{};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1) throw std::invalid_argument("Invalid IPv4 address");
    AppConfig c;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        c = m_config.getConfig();
        cameraId=allocateCameraId(c.cameras,ip,cameraId);
        // 同 IP 覆盖
        bool replaced = false;
        for (auto& bc : c.cameras) {
            if (bc.ip == ip) {
                bc.modelId = modelId;
                bc.name = name;
                bc.cameraId = cameraId;
                bc.sshUser = sshUser;
                bc.sshPass = sshPass;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            AppConfig::BoundCamera bc;
            bc.ip = ip;
            bc.modelId = modelId;
            bc.name = name;
            bc.cameraId = cameraId;
            bc.sshUser = sshUser;
            bc.sshPass = sshPass;
            c.cameras.push_back(bc);
        }
        if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
    }
    Logger::info("相机已添加到相机库: {} ({}，Camera ID {}) - 软件启动时将自动连接", ip, name, cameraId);
    return getBoundCameras();
}

nlohmann::json BridgeService::removeBoundCamera(const std::string& ip) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        AppConfig c = m_config.getConfig();
        c.cameras.erase(std::remove_if(c.cameras.begin(), c.cameras.end(),
            [&](const AppConfig::BoundCamera& bc) { return bc.ip == ip; }), c.cameras.end());
        if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
    }
    Logger::info("相机已从相机库移除: {} - 不再自动连接", ip);
    // 断开并销毁该相机的控制器（界面状态立即回到离线）
    if (CameraController* c = findCamera(ip)) {
        {std::lock_guard<std::mutex> sdkLock(m_sdkMutex);c->disconnectCamera();}
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::lock_guard<std::recursive_mutex> registryLock(m_registryMutex);
        auto it = m_cameras.find(ip);
        if (it != m_cameras.end()) { m_retired.push_back(std::move(it->second)); m_cameras.erase(it); }
        if (m_activeIp == ip) m_activeIp.clear();
    }
    return getBoundCameras();
}

nlohmann::json BridgeService::getBoundCameras() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return getBoundCamerasUnlocked();
}

bool BridgeService::renameBoundCamera(const std::string& ip, const std::string& newName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    AppConfig c = m_config.getConfig();
    for (auto& bc : c.cameras) {
        if (bc.ip == ip) {
            bc.name = newName;
            if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
            Logger::info("相机重命名: {} -> {}", ip, newName);
            return true;
        }
    }
    return false;
}

bool BridgeService::setBoundCameraId(const std::string& ip, int cameraId) {
    if (cameraId < 1 || cameraId > 15) return false;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    AppConfig c = m_config.getConfig();
    for (const auto& bc : c.cameras) if (bc.ip != ip && bc.cameraId == cameraId) return false;
    for (auto& bc : c.cameras) {
        if (bc.ip == ip) {
            bc.cameraId = cameraId;
            if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
            Logger::info("相机 {} 的 Camera ID 设为 {}", ip, cameraId);
            return true;
        }
    }
    return false;
}

nlohmann::json BridgeService::getBoundCamerasUnlocked() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& bc : m_config.getConfig().cameras) {
        arr.push_back({{"ip", bc.ip}, {"name", bc.name}, {"modelId", bc.modelId},
                       {"cameraId", bc.cameraId}});   // cameraId 必须带上，否则 UI 不同步
    }
    return arr;
}

// ---------------------------------------------------------------- polling
void BridgeService::startPolling() {
    if (m_polling) return;
    m_polling = true;
    m_pollThread = std::thread([this]() { pollLoop(); });
}

void BridgeService::stopPolling() {
    m_polling = false;
    if (m_pollThread.joinable()) m_pollThread.join();
}

void BridgeService::pollLoop() {
    while (m_polling) {
        // 向导搜索期间完全暂停后台竞争（轮询枚举 + 自动连接都占用 SDK 大锁）
        if (m_discoveryPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }
        if (!m_started || m_sdkBusy) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // 快照相机库
        std::vector<AppConfig::BoundCamera> libs;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            libs = m_config.getConfig().cameras;
        }

        // 1) 已连接相机：逐台在线探测（关机后 WiFi TCP 半开，SDK 收不到断开事件）
        for (const auto& bc : libs) {
            CameraController* c = findCamera(bc.ip);
            if (!c || !c->isConnected()) continue;
            std::string ip = c->connectedIp();
            if (ip.empty() || ip.rfind("USB:", 0) == 0) continue;
            c->refreshRecording();
            if (tcpReachable(ip, 15740, 900)) {
                m_aliveFail[bc.ip] = 0;
            } else if (++m_aliveFail[bc.ip] >= 3) {
                Logger::warn("相机 {} 连续 {} 次探测不可达（可能已关机），主动断开连接", bc.ip, m_aliveFail[bc.ip]);
                m_aliveFail[bc.ip] = 0;
                std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
                c->disconnectCamera();
            }
        }

        static size_t roundRobin=0;
        if(!libs.empty()) {roundRobin%=libs.size();std::rotate(libs.begin(),libs.begin()+roundRobin,libs.end());roundRobin=(roundRobin+1)%libs.size();}
        // 2) 未连接的库内相机：逐台自动连接（全局冷却 ≈12 秒，一次连一台）
        AppConfig::BoundCamera target;
        bool hasTarget = false;
        for (const auto& bc : libs) {
            if(!validIpv4(bc.ip))continue; // Retain legacy USB bindings without opening USB.
            CameraController* c = findCamera(bc.ip);
            bool eligible=false;
            {std::lock_guard<std::recursive_mutex> lock(m_mutex);
             eligible=(!c || !c->isConnected()) && !m_manualDisconnected[bc.ip] && std::chrono::steady_clock::now()>=m_retryAt[bc.ip];}
            if(!eligible)continue;
            if(!m_polling || m_discoveryPaused)break;
            // A silent offline host must not consume a full SDK pairing timeout.
            if(!tcpReachable(bc.ip,15740,500)) {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                m_retryAt[bc.ip]=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                m_lastError[bc.ip]="Network unreachable";continue;
            }
            {std::lock_guard<std::recursive_mutex> lock(m_mutex);
             m_retryAt[bc.ip]=std::chrono::steady_clock::now()+std::chrono::seconds(90);m_lastError[bc.ip].clear();}
            target=bc;hasTarget=true;break;
        }
        if (hasTarget && m_polling && !m_discoveryPaused) {
            const bool isUsb = target.ip.rfind("USB:", 0) == 0;
            Logger::info("自动连接相机库中的相机: {} ...", target.ip);
            BusyScope busy(m_sdkBusy);
            {
                std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
                {std::lock_guard<std::recursive_mutex> lock(m_mutex);
                 bool exists=false;for(const auto& bc:m_config.getConfig().cameras)if(bc.ip==target.ip)exists=true;
                 if(!exists || m_manualDisconnected[target.ip])continue;}
                CameraController* c = camera(target.ip);
                bool ok = false;
                if (!isUsb) ok = c->connectByIp(target.ip, target.modelId, target.sshUser, target.sshPass);
                if (ok) Logger::info("自动连接成功: {}", target.ip);
            }
    
            continue;
        }

        // 3) 发现新相机（按序列号去重）打日志提醒，并更新轻量锁保护的快照
        //    a) 有待连接的库内相机时完全跳过：正式连接优先
        //    b) 无待连接时也节流到 ≈60 秒一次：EnumCameraObjects 会对每台相机建
        //       探测性 PTP 会话获取型号/配对状态，相机侧释放会话有滞后，频繁枚举
        //       会持续占用未入库相机的会话槽 → 手动连接必报 0x20019（实测）
        if (hasTarget) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        if (m_discoveryCooldown-- > 0) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        m_discoveryCooldown = 20;   // 20 轮 × 3 秒 ≈ 60 秒
        std::vector<CameraInfo> cameras;
        {
            std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
            cameras = m_enumCam.discoverCameras(2);
        }
        std::lock_guard<std::mutex> snapLock(m_discoveredMutex);
        for (const auto& cam : cameras) {
            bool known = false;
            for (const auto& old : m_discovered) {
                if (old.serialNumber == cam.serialNumber) { known = true; break; }
            }
            if (!known) {
                Logger::info("发现相机: {} ({}) via {} - 选中后点连接，然后在相机上确认配对",
                             cam.model, cam.serialNumber, cam.connectionType);
            }
        }
        m_discovered = cameras;
    }
}

nlohmann::json BridgeService::getDiscovered() {
    std::lock_guard<std::mutex> lock(m_discoveredMutex);
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < m_discovered.size(); ++i) {
        arr.push_back({{"index", i},
                       {"model", m_discovered[i].model},
                       {"serial", m_discovered[i].serialNumber},
                       {"type", m_discovered[i].connectionType},
                       {"pairingRequired", m_discovered[i].pairingRequired}});
    }
    return arr;
}

void BridgeService::shutdown() {
    if (!m_started) return;
    stopLiveViewStream();   // 先停监看推流（避免 SDK 释放后仍在取帧）
    stopPolling();
    m_notify.stop();

    // All WebHost tasks and polling/stream threads have been joined.
    std::lock_guard<std::mutex> sdkLock(m_sdkMutex);

    disconnectAtem();
    for (auto& [ip, c] : m_cameras) {
        if (c) c->disconnectCamera();
    }
    m_cameras.clear();
    m_retired.clear();
    m_enumCam.disconnectCamera();
    m_enumCam.clearDiscovery();
    CameraController::releaseSDK();
    m_started = false;
}

void BridgeService::wireCallbacks() {
    m_atem.setProgramChangedCallback([this](int input) { onAtemProgramChanged(input); });
    m_atem.setPreviewChangedCallback([this](int input) { onAtemPreviewChanged(input); });
    m_atem.setRecordingStatusCallback([this](bool rec) { onAtemRecordingChanged(rec); });
    m_atem.setTallyChangedCallback([this](int input, bool state) { onAtemTallyChanged(input, state); });
    // 相机控制器的状态回调在 camera() 创建实例时各自注册（onCameraStatus）
}

// ---------------------------------------------------------------- status
nlohmann::json BridgeService::getStatus() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const AppConfig& cfg = m_config.getConfig();

    nlohmann::json atem = {
        {"connected", m_atem.isConnected()},
        {"ipAddress", cfg.atem.ipAddress},
        {"port", cfg.atem.port},
        {"program", m_atem.getProgramInput().index},
        {"preview", m_atem.getPreviewInput().index},
        {"recording", m_atem.getRecordingStatus().isRecording},
    };

    // 当前选中相机的状态（向后兼容字段）
    CameraController* act = findCamera(m_activeIp);
    nlohmann::json camera = {
        {"connected", act && act->isConnected()},
        {"recording", act && act->recordingKnown() && act->isRecording()},
        {"activeIp", m_activeIp},
    };


    nlohmann::json rules = nlohmann::json::array();
    for (const auto& rule : m_mapping.getRules()) {
        rules.push_back({
            {"id", rule.id},
            {"name", rule.name},
            {"enabled", rule.enabled},
            {"atemCommand", static_cast<int>(rule.atemCommand)},
        });
    }

    nlohmann::json bound = getBoundCamerasUnlocked();
    for (auto& bcJson : bound) {
        std::string ip = bcJson.value("ip", std::string());
        CameraController* c = findCamera(ip);
        bcJson["connected"] = c && c->isConnected();
        bcJson["recording"] = c && c->recordingKnown() && c->isRecording();
        bcJson["recordingKnown"] = c && c->recordingKnown();
        int modelId=bcJson.value("modelId",8);
        bcJson["model"] = modelId>=0 && modelId<(int)CameraController::modelNames().size() ? CameraController::modelNames()[modelId] : "Unknown";
        if(c && c->isConnected()) {auto info=c->getCameraInfo();if(!info.model.empty())bcJson["model"]=info.model;}
        bcJson["lastError"] = c ? c->lastError() : 0;
        bcJson["lastErrorMessage"]=m_lastError[ip];
        bcJson["transportEnabled"]=validIpv4(ip);
        bcJson["phase"] = c && c->isConnected() ? "online" : (c && c->connectionState()==CameraStatus::CONNECTING ? "connecting" : (m_manualDisconnected[ip] ? "offline" : (c && c->lastError() ? "failed" : "retry_wait")));
    }

    return {
        {"app", {
            {"name", "ATEM Sony Bridge"},
            {"version", BRIDGE_VERSION},
            {"sdkInitialized", CameraController::isSDKInitialized()},
        }},
        {"atem", atem},
        {"camera", camera},
        {"rules", rules},
        {"ruleCount", m_mapping.getRules().size()},
        {"logLevel", m_config.getConfig().log.level},
        {"boundCameras", bound},
    };
}

nlohmann::json BridgeService::getConfig() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const AppConfig& c = m_config.getConfig();
    return {
        {"atem", {
            {"ipAddress", c.atem.ipAddress},
            {"port", c.atem.port},
            {"autoConnect", c.atem.autoConnect},
        }},
        {"camera", {
            {"autoDiscover", c.camera.autoDiscover},
            {"discoveryTimeout", c.camera.discoveryTimeout},
        }},
        {"log", {
            {"level", c.log.level},
            {"filePath", c.log.filePath},
        }},
        {"mapping", {
            {"rulesFile", c.mapping.rulesFile},
            {"autoLoadRules", c.mapping.autoLoadRules},
        }},
    };
}

bool BridgeService::applyConfig(const nlohmann::json& cfg) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        AppConfig c = m_config.getConfig();
        if (cfg.contains("atem")) {
            auto& a = cfg["atem"];
            if (a.contains("ipAddress")) c.atem.ipAddress = std::string(a["ipAddress"]);
            if (a.contains("port")) c.atem.port = a["port"].get<int>();
        }
        if (cfg.contains("camera")) {
            auto& cam = cfg["camera"];
            if (cam.contains("discoveryTimeout")) c.camera.discoveryTimeout = cam["discoveryTimeout"].get<int>();
        }
        if (cfg.contains("log") && cfg["log"].contains("level")) {
            c.log.level = std::string(cfg["log"]["level"]);
        }
        if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
    }
    Logger::info("Configuration updated via UI");
    return true;
}

nlohmann::json BridgeService::getLogs() {
    auto entries = Logger::recentEntries(200);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        arr.push_back({{"time", e.timeStr}, {"level", static_cast<int>(e.level)}, {"text", e.text}});
    }
    return arr;
}

// ---------------------------------------------------------------- atem
nlohmann::json BridgeService::connectAtem(const std::string&, int) {
    return {{"ok", false}, {"code", "NOT_IMPLEMENTED"}, {"message", "ATEM integration is planned for 0.3.0"}};
}

nlohmann::json BridgeService::disconnectAtem() {
    m_atem.disconnect();
    return {{"ok", true}, {"message", "Disconnected"}};
}

// ---------------------------------------------------------------- camera
std::vector<CameraInfo> BridgeService::discoverCamerasSync(int timeoutSec) {
    std::vector<CameraInfo> cameras;
    {
        BusyScope busy(m_sdkBusy);
        std::lock_guard<std::mutex> lock(m_sdkMutex);
        cameras = m_enumCam.discoverCameras(timeoutSec);
        std::lock_guard<std::mutex> snapLock(m_discoveredMutex);
        m_discovered = cameras;

    }
    return cameras;
}

std::vector<CameraInfo> BridgeService::notifyEndpoints() const {
    std::vector<CameraInfo> out;
    for (const auto& e : m_notify.endpoints()) {
        CameraInfo info;
        info.model = "Sony 相机";
        info.serialNumber = e.uuid.empty() ? e.ip : e.uuid;
        info.connectionType = "NOTIFY";
        info.ipAddress = e.ip;
        out.push_back(info);
    }
    return out;
}

nlohmann::json BridgeService::discoverCameras() {
    std::vector<CameraInfo> cameras;
    {
        BusyScope busy(m_sdkBusy);
        std::lock_guard<std::mutex> lock(m_sdkMutex);
        cameras = m_enumCam.discoverCameras(m_config.getConfig().camera.discoveryTimeout);
        std::lock_guard<std::mutex> snapLock(m_discoveredMutex);
        m_discovered = cameras;

    }
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < cameras.size(); ++i) {
        arr.push_back({{"index", i}, {"model", cameras[i].model},
                       {"serial", cameras[i].serialNumber},
                       {"type", cameras[i].connectionType},
                       {"pairingRequired", cameras[i].pairingRequired}});
    }
    return {{"ok", true}, {"cameras", arr}};
}

void BridgeService::setSshCredentials(const std::string& user, const std::string& pass) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_sshUser = user;
    m_sshPass = pass;
    if (!user.empty()) Logger::info("SSH credentials set for user: {}", user);
}

nlohmann::json BridgeService::connectCamera(int discoveryIndex,
                                            const std::string& sshUser, const std::string& sshPass) {
    CameraInfo info;
    { std::lock_guard<std::mutex> lock(m_discoveredMutex);
      if(discoveryIndex<0 || discoveryIndex>=(int)m_discovered.size())
          return {{"ok",false},{"code","INVALID_INDEX"},{"message","Discovery entry expired; search again"}};
      info=m_discovered[discoveryIndex]; }
    if(info.ipAddress.empty()) return {{"ok",false},{"code","NOT_IMPLEMENTED"},{"message","USB is not available in this version"}};
    return connectCameraByIp(info.ipAddress,m_defaultModelId,sshUser,sshPass);
}

nlohmann::json BridgeService::connectCameraByIp(const std::string& ip, int modelId,
                                                const std::string& sshUser, const std::string& sshPass) {
    if(!validIpv4(ip) || modelId<0 || modelId>=(int)CameraController::modelNames().size())
        return {{"ok",false},{"code","INVALID_TARGET"},{"message","Invalid IPv4 address or camera model"}};
    bool ok;
    uint32_t error=0;
    std::string su, sp;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);   // 短锁：仅取凭据副本，连接期间不持锁（避免阻塞 UI 刷新）
        su = !sshUser.empty() ? sshUser : m_sshUser;
        sp = !sshPass.empty() ? sshPass : m_sshPass;
        for(const auto& bc:m_config.getConfig().cameras)if(bc.ip==ip){if(sshUser.empty())su=bc.sshUser;if(sshPass.empty())sp=bc.sshPass;}
    }
    {
        BusyScope busy(m_sdkBusy);
        std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
        { std::lock_guard<std::recursive_mutex> lock(m_mutex); m_manualDisconnected[ip] = false; }
        auto* target = camera(ip);
        ok = target->isConnected() || target->connectByIp(ip, modelId, su, sp);
        error=target->lastError();

    }
    return {
        {"ok", ok},
        {"code", ok ? "OK" : "CAMERA_CONNECT_FAILED"},
        {"sdkError",error},
        {"message", ok ? "Camera connected" : "Connection failed at " + ip + " (SDK " + std::to_string(error) + "). Check camera pairing and access authentication (SSH)."},
    };
}

nlohmann::json BridgeService::disconnectCamera(const std::string& ip) {
    CameraController* c = findCamera(ip.empty() ? activeCameraIp() : ip);
    { std::lock_guard<std::recursive_mutex> lock(m_mutex); m_manualDisconnected[ip.empty() ? activeCameraIp() : ip] = true; }
    bool ok = c && c->disconnectCamera();
    return {{"ok", ok}, {"message", ok ? "Camera disconnected" : "Disconnect failed"}};
}

nlohmann::json BridgeService::startRecording(const std::string& ip) {
    CameraController* c = camera(ip);
    bool ok = c && c->startRecording();
    return {{"ok", ok}, {"message", ok ? "Recording started" : "Failed to start recording"}};
}

nlohmann::json BridgeService::stopRecording(const std::string& ip) {
    CameraController* c = camera(ip);
    bool ok = c && c->stopRecording();
    return {{"ok", ok}, {"message", ok ? "Recording stopped" : "Failed to stop recording"}};
}

// ---------------- 图像回传监看（LiveView 推流） ----------------
static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out += B64_TABLE[(n >> 18) & 0x3F]; out += B64_TABLE[(n >> 12) & 0x3F];
        out += B64_TABLE[(n >> 6) & 0x3F];  out += B64_TABLE[n & 0x3F];
        i += 3;
    }
    if (i + 1 == data.size()) {
        uint32_t n = data[i] << 16;
        out += B64_TABLE[(n >> 18) & 0x3F]; out += B64_TABLE[(n >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8);
        out += B64_TABLE[(n >> 18) & 0x3F]; out += B64_TABLE[(n >> 12) & 0x3F];
        out += B64_TABLE[(n >> 6) & 0x3F];  out += '=';
    }
    return out;
}

// 单相机推流循环（独立线程）：与其它相机完全并行取帧。
// 旧版单线程串行遍历两台相机时，每台取帧是同步 SDK 调用（30-50ms），
// 排队后每路实际只有 ~9fps；并行化 + 节流提到 15fps 后每路可跑满。
void BridgeService::liveViewWorker(const std::string& ip) {
    constexpr auto kFrameInterval = std::chrono::milliseconds(66);   // ≈15fps/路
    auto lastPush = std::chrono::steady_clock::now() - kFrameInterval;
    int fpsCnt = 0, fpsVal = 0;
    auto fpsTick = std::chrono::steady_clock::now();
    bool qualityApplied = false;

    Logger::info("监看推流线程启动: {}", ip);
    while (m_lvRunning) {
        if (m_discoveryPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }

        CameraController* cam = findCamera(ip);
        if (!cam || !cam->isConnected()) break;   // 相机断开：退出，由管理线程回收

        auto now = std::chrono::steady_clock::now();
        if (now - lastPush < kFrameInterval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto frame = cam->getLiveViewFrame();
        if (frame.size() < 4) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
        lastPush = std::chrono::steady_clock::now();

        // 首帧前应用画质设置（分辨率档 + 压缩质量），并置位 LiveView
        if (!qualityApplied) {
            int res = 1, comp = -1;
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                res = m_config.getConfig().liveview.imageQuality;
                comp = m_config.getConfig().liveview.compression;
        for(const auto& bc:m_config.getConfig().cameras) if(bc.ip==ip) {if(bc.imageQuality>=0)res=bc.imageQuality;if(bc.compression>=-1)comp=bc.compression;}
            }
            if (cam->startLiveView())
                Logger::info("相机 {} LiveViewStatus 置位成功", ip);
            if (cam->setProperty(0x0508, res ? 1 : 0))
                Logger::info("相机 {} LiveView 画质档: {} ({}px)", ip, res ? "High" : "Low", res ? 1024 : 640);
            CameraProperty q = cam->getProperty(0x02FF);
            int target = comp;
            if (!q.possibleValues.empty() &&
                (target < 0 || target < q.possibleValues.front() || target > q.possibleValues.back())) {
                target = q.possibleValues.back();
            }
            if (target >= 0 && target != q.currentValue && cam->setProperty(0x02FF, target))
                Logger::info("相机 {} LiveView 压缩质量: {}", ip, target);
            qualityApplied = true;
        }

        // fps 统计（每秒刷新）
        ++fpsCnt;
        if (now - fpsTick >= std::chrono::seconds(1)) {
            fpsVal = fpsCnt;
            fpsCnt = 0;
            fpsTick = now;
        }

        if (m_eventCallback) {
            m_eventCallback(nlohmann::json{
                {"event", "liveview"},
                {"ip", ip},
                {"fps", fpsVal},
                {"frame", std::string("data:image/jpeg;base64,") + base64Encode(frame)},
            }.dump());
        }
    }
    Logger::info("监看推流线程退出: {}", ip);
}

// 推流管理线程：为每台"连接中"的相机维护一个独立 worker 线程，
// 相机断开时回收其线程（worker 自行退出后 join）
void BridgeService::liveViewLoop() {
    Logger::info("监看推流已开启（全部已连接相机，每路独立线程）");

    while (m_lvRunning) {
        if (m_discoveryPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }

        // 快照当前连接中的相机 IP
        std::vector<std::string> ips;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            std::lock_guard<std::recursive_mutex> registryLock(m_registryMutex);
            for (auto& [ip, c] : m_cameras)
                if (c && c->isConnected()) ips.push_back(ip);
        }

        // 为新连接的相机启动 worker
        for (const auto& ip : ips) {
            if (!m_lvWorkers.count(ip))
                m_lvWorkers[ip] = std::thread([this, ip]() { liveViewWorker(ip); });
        }
        // 回收已断开相机的 worker
        for (auto it = m_lvWorkers.begin(); it != m_lvWorkers.end();) {
            bool alive = std::find(ips.begin(), ips.end(), it->first) != ips.end();
            if (alive) { ++it; continue; }
            if (it->second.joinable()) it->second.join();
            it = m_lvWorkers.erase(it);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (auto& [ip, t] : m_lvWorkers) {
        if (t.joinable()) t.join();
    }
    m_lvWorkers.clear();
    std::vector<CameraController*> snapshot;
    {std::lock_guard<std::recursive_mutex> lock(m_registryMutex);for(auto& entry:m_cameras)snapshot.push_back(entry.second.get());}
    for(auto* c:snapshot) if(c)c->stopLiveView();
    Logger::info("监看推流已停止");
}

void BridgeService::startLiveViewStreams() {
    if (m_lvRunning) return;
    m_lvRunning = true;
    m_lvThread = std::thread([this]() { liveViewLoop(); });
}

void BridgeService::stopLiveViewStream() {
    if (!m_lvRunning) return;
    m_lvRunning = false;
    if (m_lvThread.joinable()) m_lvThread.join();
}

// 监看画质设置：持久化到 config，推流中则立即下发相机
nlohmann::json BridgeService::setLiveviewQuality(const std::string& ip, int imageQuality, int compression) {
    if(imageQuality<0 || imageQuality>1 || compression < -1 || compression>100) throw std::invalid_argument("Invalid liveview quality");
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        AppConfig c = m_config.getConfig();
        if(ip.empty()) { c.liveview.imageQuality=imageQuality;c.liveview.compression=compression; }
        else {bool found=false;for(auto& bc:c.cameras) if(bc.ip==ip) {bc.imageQuality=imageQuality;bc.compression=compression;found=true;}
            if(!found) throw std::invalid_argument("Camera is not in library");}
        if(!m_config.commitConfig(c,resourcePath("config/config.json"))) throw std::runtime_error("Configuration save failed; previous file retained");
    }
    // 推流中立即应用（写入较快，RPC 线程可接受）
    bool applied = false;
    std::string target = ip;
    CameraController* cam = target.empty() ? nullptr : findCamera(target);
    if (cam && cam->isConnected()) {
        int res = 1, comp = -1;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            res = m_config.getConfig().liveview.imageQuality;
            comp = m_config.getConfig().liveview.compression;
        for(const auto& bc:m_config.getConfig().cameras) if(bc.ip==ip) {if(bc.imageQuality>=0)res=bc.imageQuality;if(bc.compression>=-1)comp=bc.compression;}
        }
        std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
        applied = cam->setProperty(0x0508, res ? 1 : 0);
        if (comp >= 0) cam->setProperty(0x02FF, comp);
        Logger::info("监看画质已更新: 分辨率档 {} / 压缩质量 {}", res ? "High" : "Low", comp);
    }
    nlohmann::json out = { {"ok", true}, {"applied", applied} };
    // 附上压缩质量的合法范围（已连接时），供前端滑块定位
    if (cam && cam->isConnected()) {
        CameraProperty q = cam->getProperty(0x02FF);
        if (q.possibleValues.size() >= 2) {
            out["min"] = q.possibleValues.front();
            out["max"] = q.possibleValues.back();
            out["current"] = q.currentValue;
        }
    }
    return out;
}

// 查询监看画质设置（配置 + 相机压缩质量范围）
nlohmann::json BridgeService::getLiveviewQuality(const std::string& ip) {
    int res = 1, comp = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        res = m_config.getConfig().liveview.imageQuality;
        comp = m_config.getConfig().liveview.compression;
        for(const auto& bc:m_config.getConfig().cameras) if(bc.ip==ip) {if(bc.imageQuality>=0)res=bc.imageQuality;if(bc.compression>=-1)comp=bc.compression;}
    }
    nlohmann::json out = { {"imageQuality", res}, {"compression", comp} };
    CameraController* cam = findCamera(ip);
    if (cam && cam->isConnected()) {
        std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
        CameraProperty q = cam->getProperty(0x02FF);
        if (q.possibleValues.size() >= 2) {
            out["min"] = q.possibleValues.front();
            out["max"] = q.possibleValues.back();
            out["current"] = q.currentValue;
            if (comp < 0) out["effective"] = q.possibleValues.back();
        }
    }
    return out;
}

nlohmann::json BridgeService::getProperties(const std::string& ip) {
    nlohmann::json arr = nlohmann::json::array();
    CameraController* cam = findCamera(ip.empty() ? activeCameraIp() : ip);
    if (!cam || !cam->isConnected()) return {{"ok", false}, {"properties", arr}};
    auto props = cam->getCameraProperties();
    // 调试日志：相机上报的全部属性码（确认特定属性是否存在，如 Tally 0x0300-0x0302）
    {
        std::string codes;
        for (const auto& [code, prop] : props) codes += "0x" + [&] { char b[8]; snprintf(b, 8, "%04X", code); return std::string(b); }() + " ";
        Logger::info("相机属性列表({} 项): {}", props.size(), codes);
    }
    for (const auto& [code, prop] : props) {
        nlohmann::json p = {
            {"code", code},
            {"value", exactInteger(prop.currentValue,prop.valueType=="UInt64")}, {"readable", prop.readable}, {"writable", prop.writable}, {"range",prop.range},
        };
        nlohmann::json poss = nlohmann::json::array();
        for (auto v : prop.possibleValues) poss.push_back(exactInteger(v,prop.valueType=="UInt64"));
        p["possible"] = poss;
        arr.push_back(p);
    }
    return {{"ok", true}, {"properties", arr}};
}

nlohmann::json BridgeService::setProperty(const std::string& ip, uint32_t code, int64_t value) {
    CameraController* cam = camera(ip);
    if(value>9007199254740991LL || value < -9007199254740991LL) return {{"ok",false},{"code","UNSAFE_INTEGER"},{"message","Value exceeds exact JSON numeric range"}};
    bool ok = cam && cam->setProperty(code, value);
    auto actual = cam->getProperty(code);
    bool verified=ok && actual.readable && actual.currentValue==value;
    return {{"ok",verified},{"submitted",ok},{"value",exactInteger(actual.currentValue)},
            {"code",verified?"OK":ok?"READBACK_UNCONFIRMED":"PROPERTY_REJECTED"},
            {"message",verified?"Property readback confirmed":ok?"Command submitted; readback does not yet confirm requested value":"Camera rejected property write"}};
}

// ---------------------------------------------------------------- rules
nlohmann::json BridgeService::getRules() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rule : m_mapping.getRules()) {
        nlohmann::json r = {
            {"id", rule.id},
            {"name", rule.name},
            {"type", static_cast<int>(rule.type)},
            {"atemCommand", static_cast<int>(rule.atemCommand)},
            {"enabled", rule.enabled},
        };
        nlohmann::json conds = nlohmann::json::array();
        for (const auto& c : rule.conditions) {
            conds.push_back({{"type", c.type}, {"parameter", c.parameter},
                             {"value", c.value}, {"comparison", c.comparison}});
        }
        r["conditions"] = conds;
        nlohmann::json actions = nlohmann::json::array();
        for (const auto& a : rule.actions) {
            actions.push_back({{"command", a.command}, {"value", a.value}, {"delayMs", a.delayMs}});
        }
        r["actions"] = actions;
        arr.push_back(r);
    }
    return arr;
}

bool BridgeService::saveRules(const nlohmann::json& rulesArray) {
    if (!rulesArray.is_array()) return false;

    // Rebuild all rules from the submitted array
    auto existing = m_mapping.getRules();
    for (const auto& r : existing) m_mapping.removeRule(r.id);

    for (const auto& rj : rulesArray) {
        MappingRule rule;
        rule.id = rj.value("id", std::string("rule-") + std::to_string(existing.size()));
        rule.name = rj.value("name", std::string("unnamed"));
        if (rj.contains("atemCommand")) rule.atemCommand = static_cast<AtemCommand>(rj["atemCommand"].get<int>());
        rule.enabled = rj.value("enabled", true);
        if (rj.contains("conditions")) {
            for (const auto& cj : rj["conditions"]) {
                MappingCondition c;
                c.type = cj.value("type", std::string());
                c.parameter = cj.value("parameter", std::string());
                c.value = cj.value("value", std::string());
                c.comparison = cj.value("comparison", std::string("equals"));
                rule.conditions.push_back(c);
            }
        }
        if (rj.contains("actions")) {
            for (const auto& aj : rj["actions"]) {
                MappingAction a;
                a.command = aj.value("command", std::string());
                a.parameter = aj.value("parameter", std::string());
                a.value = aj.value("value", std::string());
                a.delayMs = aj.value("delayMs", 0);
                rule.actions.push_back(a);
            }
        }
        m_mapping.addRule(rule);
    }

    bool saved = m_mapping.saveRulesToFile(resourcePath("config/" + m_config.getRulesFile()));
    if (saved) Logger::info("Saved {} mapping rules via UI", m_mapping.getRules().size());
    return saved;
}

bool BridgeService::toggleRule(const std::string& ruleId, bool enabled) {
    try {
        MappingRule rule = m_mapping.getRule(ruleId);
        rule.enabled = enabled;
        m_mapping.updateRule(rule);
        m_mapping.saveRulesToFile(resourcePath("config/" + m_config.getRulesFile()));
        Logger::info("Rule {} {}", ruleId, enabled ? "enabled" : "disabled");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool BridgeService::removeRule(const std::string& ruleId) {
    try {
        m_mapping.getRule(ruleId); // throws when missing
        m_mapping.removeRule(ruleId);
        m_mapping.saveRulesToFile(resourcePath("config/" + m_config.getRulesFile()));
        Logger::info("Removed rule {}", ruleId);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ------------------------------------------------------- atem event handlers
void BridgeService::onAtemProgramChanged(int input) {
    Logger::info("ATEM program changed to input {}", input);
    m_mapping.processAtemCommand(AtemCommand::PROGRAM_CHANGE, {std::to_string(input)},nullptr);
}

void BridgeService::onAtemPreviewChanged(int input) {
    Logger::info("ATEM preview changed to input {}", input);
    m_mapping.processAtemCommand(AtemCommand::PREVIEW_CHANGE, {std::to_string(input)},nullptr);
}

void BridgeService::onAtemRecordingChanged(bool recording) {
    Logger::info("ATEM recording: {}", recording ? "started" : "stopped");
    m_mapping.processAtemCommand(recording ? AtemCommand::RECORD_START : AtemCommand::RECORD_STOP, {},nullptr);
}

void BridgeService::onAtemTallyChanged(int input, bool state) {
    Logger::debug("ATEM tally input {} = {}", input, state);
    m_mapping.processAtemCommand(AtemCommand::TALLY_CHANGE,
        {std::to_string(input), state ? "1" : "0"},nullptr);
}
