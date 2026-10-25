#include "CameraController.h"
#include "common/Logger.h"
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include "common/FrameUtils.h"
#include "common/Validation.h"
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

// ---------- wide <-> UTF-8 helpers (CrChar is wchar_t on Windows) ----------
static std::string wideToUtf8(const CrChar* wstr) {
    if (!wstr) return "";
#ifdef _WIN32
    size_t len = wcslen(wstr);
    if (len == 0) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, (int)len, nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, (int)len, &out[0], size, nullptr, nullptr);
    return out;
#else
    return std::string(wstr);
#endif
}

static std::string deviceIdToString(const SCRSDK::ICrCameraObjectInfo* info) {
    std::ostringstream oss;
    CrInt8u* id = info->GetId();
    CrInt32u size = info->GetIdSize();
    if (info->GetIdType() == SCRSDK::CAMERAOBJECTID_TYPE_IP) {
        // 优先用 SDK 提供的字符串形式（避免字节序歧义）
        std::string ipStr = wideToUtf8(info->GetIPAddressChar());
        if (!ipStr.empty()) {
            oss << ipStr;
        } else if (size >= 4 && id) {
            oss << (int)id[0] << "." << (int)id[1] << "." << (int)id[2] << "." << (int)id[3];
        }
    } else if (info->GetIdType() == SCRSDK::CAMERAOBJECTID_TYPE_USB && id) {
        // serial number is typically an ASCII/wide string
        CrChar* serial = reinterpret_cast<CrChar*>(id);
        oss << wideToUtf8(serial);
        if (oss.str().empty()) {
            for (CrInt32u i = 0; i < size; ++i) {
                if (id[i] >= 0x20 && id[i] < 0x7F) oss << (char)id[i];
            }
        }
    }
    return oss.str();
}

// 释放 ICrCameraObjectInfo 的自定义删除器
struct ObjectInfoReleaser {
    void operator()(SCRSDK::ICrCameraObjectInfo* p) const { if (p) p->Release(); }
};

// ---------- implementation class ----------
class CameraControllerImpl : public SCRSDK::IDeviceCallback {
public:
    CameraControllerImpl() : m_deviceHandle(0), m_connected(false), m_recording(false), m_connecting(false) {}

    ~CameraControllerImpl() {}

    // IDeviceCallback interface
    void OnConnected(SCRSDK::DeviceConnectionVersioin version) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected = true;
        m_connecting = false;
        m_cv.notify_all();
        Logger::info("Camera connected (protocol v{})", (int)version);


    }

    void OnDisconnected(CrInt32u error) override {
        bool wasConnected;
        {
        std::lock_guard<std::mutex> lock(m_mutex);
        wasConnected = m_connected;
        lastError=error;
        recordingKnown=false;
        m_connected = false;
        m_recording = false;
        m_connecting = false;
        m_connectedIp.clear();
        }
        Logger::warn("Camera disconnected (was connected: {}, error: 0x{:x})", wasConnected, error);
        m_cv.notify_all();


    }

    void OnError(CrInt32u error) override {
        lastError=error;
        m_connecting=false;
        Logger::error("Camera error: 0x{:x}", error);
        m_cv.notify_all(); // may unblock a pending connect
    }

    void OnPropertyChanged() override {
        Logger::debug("Camera property changed");
        auto cb = m_propertyCallback;
        if (cb) cb(0, 0); // generic notification; controller re-reads properties
    }

    void OnLvPropertyChanged() override {
        Logger::debug("Live view property changed");
    }

    SCRSDK::CrDeviceHandle m_deviceHandle;
    std::atomic<bool> m_connected;
    std::atomic<bool> m_connecting;
    std::atomic<bool> m_recording;
    std::atomic<bool> recordingKnown{false};
    std::atomic<uint32_t> lastError{0};
    CameraInfo connectedInfo;
    std::string m_connectedIp;   // 当前连接相机的 IP（USB 为空；断开时清空）
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::vector<CameraInfo> m_discovery;
    struct EnumReleaser { void operator()(SCRSDK::ICrEnumCameraObjectInfo* p) const { if(p) p->Release(); } };
    std::unique_ptr<SCRSDK::ICrEnumCameraObjectInfo, EnumReleaser> m_lastEnum;
    std::recursive_mutex operationMutex;
    std::atomic<int> controlWaiting{0};
    struct ControlGuard {
        CameraControllerImpl& owner;
        explicit ControlGuard(CameraControllerImpl& o):owner(o){++owner.controlWaiting;owner.operationMutex.lock();}
        ~ControlGuard(){owner.operationMutex.unlock();--owner.controlWaiting;}
    };
    std::unique_ptr<SCRSDK::ICrCameraObjectInfo, ObjectInfoReleaser> m_manualInfo; // 手动 IP 连接的设备对象
    std::string m_manualIp;      // 缓存对象对应的 IP
    int m_manualModel = -1;      // 缓存对象对应的型号

    std::function<void(uint32_t, int64_t)> m_propertyCallback;
    std::function<void(CameraStatus)> m_statusCallback;
};

// Static SDK initialization
static bool s_sdkInitialized = false;
static std::mutex s_sdkMutex;

CameraController::CameraController() : m_impl(new CameraControllerImpl()) {
}

CameraController::~CameraController() {
    if (m_impl->m_deviceHandle || m_impl->m_connected || m_impl->m_connecting) {
        disconnectCamera();
    }
    // 设备对象必须先于 SCRSDK::Release 释放，否则其 Release 会调入
    // 已卸载的 Cr_PTP_IP.dll（0xc0000005 崩溃）
    m_impl->m_manualInfo.reset();
    m_impl->m_lastEnum.reset();
}

bool CameraController::initializeSDK() {
    std::lock_guard<std::mutex> lock(s_sdkMutex);
    if (s_sdkInitialized) {
        return true;
    }

    // Init with append-mode log file (CrDebug_DLL implies file logging in SDK)
    bool ok = SCRSDK::Init(0);
    if (!ok) {
        Logger::error("Failed to initialize Sony SDK");
        return false;
    }

    s_sdkInitialized = true;
    Logger::info("Sony SDK initialized successfully (v0x{:x})", SCRSDK::GetSDKVersion());
    return true;
}

void CameraController::releaseSDK() {
    std::lock_guard<std::mutex> lock(s_sdkMutex);
    if (!s_sdkInitialized) {
        return;
    }

    SCRSDK::Release();
    s_sdkInitialized = false;
    Logger::info("Sony SDK released");
}

bool CameraController::isSDKInitialized() {
    std::lock_guard<std::mutex> lock(s_sdkMutex);
    return s_sdkInitialized;
}

std::vector<CameraInfo> CameraController::discoverCameras(int timeoutSec) {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    std::vector<CameraInfo> cameras;
    Logger::info("Discovering cameras (timeout: {}s)...", timeoutSec);

    SCRSDK::ICrEnumCameraObjectInfo* enumInfo = nullptr;
    SCRSDK::CrError err = SCRSDK::EnumCameraObjects(&enumInfo, (CrInt8u)timeoutSec);
    if (err != SCRSDK::CrError_None || !enumInfo) {
        Logger::warn("Camera enumeration failed or no cameras found (0x{:x})", err);
        return cameras;
    }

    m_impl->m_lastEnum.reset(enumInfo);

    CrInt32u count = enumInfo->GetCount();
    Logger::info("Found {} camera(s)", count);

    for (CrInt32u i = 0; i < count; ++i) {
        const SCRSDK::ICrCameraObjectInfo* info = enumInfo->GetCameraObjectInfo(i);
        if (!info) continue;

        CameraInfo cam;
        cam.name = wideToUtf8(info->GetName());
        cam.model = wideToUtf8(info->GetModel());
        cam.serialNumber = deviceIdToString(info);
        cam.connectionType = wideToUtf8(info->GetConnectionTypeName());
        if (cam.connectionType.empty()) {
            cam.connectionType = info->GetIPAddress() != 0 ? "Ethernet" : "USB";
        }
        cam.ipAddress = wideToUtf8(info->GetIPAddressChar());
        cam.pairingRequired = info->GetPairingNecessity() && wcslen(info->GetPairingNecessity()) > 0;
        cam.cameraObject = info;

        Logger::info("  [{}] {} ({}) via {} {}", i, cam.model, cam.serialNumber,
                     cam.connectionType,
                     cam.pairingRequired ? "- pairing required!" : "");
        cameras.push_back(cam);
    }

    m_impl->m_discovery = cameras;
    return cameras;
}

std::vector<CameraInfo> CameraController::lastDiscovery() const {
    return m_impl->m_discovery;
}

bool CameraController::connectToCamera(int discoveryIndex, const std::string& sshUser, const std::string& sshPass) {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    auto& discovery = m_impl->m_discovery;
    if (discoveryIndex < 0 || discoveryIndex >= (int)discovery.size()) {
        Logger::error("Invalid camera index {} (discovered: {})", discoveryIndex, discovery.size());
        return false;
    }
    return connectToCamera(discovery[discoveryIndex], sshUser, sshPass);
}

bool CameraController::connectToCamera(const CameraInfo& camera,
                                       const std::string& sshUser, const std::string& sshPass) {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    if (m_impl->m_connected || m_impl->m_connecting) {
        Logger::warn("Already connected/connecting; disconnect first");
        return false;
    }
    if (!camera.cameraObject) {
        Logger::error("Cannot connect: camera object is null (run discoverCameras first)");
        return false;
    }
    bool ok = finishConnect(camera.cameraObject, camera.model, sshUser, sshPass);
    if (ok) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_connectedIp = camera.ipAddress;
    }
    return ok;
}

// 按 IP 手动连接（绕过 SSDP 自动发现；相机需处于远程控制模式）
bool CameraController::connectByIp(const std::string& ip, int modelId,
                                   const std::string& sshUser, const std::string& sshPass) {
    if(!validIpv4(ip)) return false;
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    if (m_impl->m_connected || m_impl->m_connecting) {
        Logger::warn("Already connected/connecting; disconnect first");
        return false;
    }

    if(modelId<0 || modelId>=(int)modelNames().size()) return false;
    if(m_impl->m_deviceHandle) disconnectCamera();
    // ===== 官方多机连接路径（connectMulti.cpp / monitoring.cpp 均如此）=====
    // 连接对象必须来自 EnumCameraObjects：枚举对象携带相机真实身份（MAC 等）。
    // 手工构造的对象（CreateCameraObjectInfoEthernetConnection，MAC 全零）身份字段
    // 不完整——多台相机同时连接时 SDK 会把第二台判定为"超过可连接数"
    // （Connect 返回警告 0x20019 OverLimitOfDevice），实测第二台永远连不上。
    // 因此优先枚举并按 IP 匹配；SSDP 受限枚举不到时才退回手工构造。
    {
        std::vector<CameraInfo> cams = discoverCameras(2);
        for (const auto& cam : cams) {
            if (cam.ipAddress == ip && cam.cameraObject) {
                Logger::info("按 IP 匹配到枚举对象（官方多机路径）: {} {}", ip, cam.model);
                bool ok = finishConnect(cam.cameraObject, ip, sshUser, sshPass);
                // 配对流程：首次连接触发配对，相机确认后断开本会话等待重连
                if (!ok && !m_impl->m_connected) {
                    Logger::info("首次连接未建立（等待配对确认）- 8 秒后自动重连");
                    std::this_thread::sleep_for(std::chrono::seconds(8));
                    ok = finishConnect(cam.cameraObject, ip + " (retry)", sshUser, sshPass);
                }
                if (ok) {
                    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
                    m_impl->m_connectedIp = ip;
                }
                return ok;
            }
        }
        Logger::warn("枚举未发现 {}（相机刚上线或 SSDP 受限），退回手工构造对象连接", ip);
    }

    // 解析 "10.0.0.177" → CrInt32u（按官方示例的小端字节序打包）
    CrInt32u ipAddress = 0;
    {
        int octets[4] = { 0, 0, 0, 0 };
        int count = 0;
        size_t start = 0;
        while (count < 4 && start <= ip.size()) {
            size_t dot = ip.find('.', start);
            std::string part = (dot == std::string::npos) ? ip.substr(start) : ip.substr(start, dot - start);
            try { octets[count++] = std::stoi(part); } catch (const std::exception&) { break; }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (count != 4) {
            Logger::error("Invalid IP address: {}", ip);
            return false;
        }
        for(int n:octets) if(n<0 || n>255) return false;
        for (int i = 0; i < 4; ++i) ipAddress |= (CrInt32u)(octets[i] & 0xFF) << (i * 8);
    }

    // 同 ip+model 复用已缓存的设备对象：GUID 不变，相机配对一次后重连免配对
    if (m_impl->m_manualInfo && m_impl->m_manualIp == ip && m_impl->m_manualModel == modelId) {
        Logger::info("Reusing cached camera object for {} (already paired)", ip);
        bool ok = finishConnect(m_impl->m_manualInfo.get(), ip, sshUser, sshPass);
        if (ok) {
            std::lock_guard<std::mutex> lock(m_impl->m_mutex);
            m_impl->m_connectedIp = ip;
        }
        return ok;
    }

    CrInt8u macAddress[6] = { 0 };
    SCRSDK::ICrCameraObjectInfo* objInfo = nullptr;
    SCRSDK::CrError err = SCRSDK::CreateCameraObjectInfoEthernetConnection(
        &objInfo, (SCRSDK::CrCameraDeviceModelList)modelId, ipAddress, macAddress,
        sshUser.empty() ? 0 : 1);  // SSHsupport 标志：提供凭据即声明走 SSH 通道
    if (err != SCRSDK::CrError_None || !objInfo) {
        Logger::error("CreateCameraObjectInfoEthernetConnection failed: 0x{:x}", err);
        return false;
    }
    m_impl->m_manualInfo.reset(objInfo);
    m_impl->m_manualIp = ip;
    m_impl->m_manualModel = modelId;

    Logger::info("Manual connect to {} (model: {})", ip, modelNames().empty() ? "?" : modelNames()[modelId < (int)modelNames().size() ? modelId : 0]);
    bool ok = finishConnect(objInfo, ip, sshUser, sshPass);

    // 配对流程特性：首次连接触发配对，相机确认后会主动断开本次会话，
    // 等待已配对主机重连。用同一设备对象（同 GUID）自动重连即可直连。
    // 间隔 8 秒：若上一次 Connect 被 0x20019 拒绝，相机侧会话释放有滞后，
    // 太快重试会再次撞上（OverLimitOfDevice）
    if (!ok && !m_impl->m_connected) {
        Logger::info("首次连接未建立（等待配对确认）- 8 秒后自动用同一设备对象重连");
        std::this_thread::sleep_for(std::chrono::seconds(8));
        ok = finishConnect(m_impl->m_manualInfo.get(), ip + " (retry)", sshUser, sshPass);
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_connectedIp = ip;
    }
    return ok;
}

// 连接收尾：发起 Connect 并同步等待 OnConnected 回调
// WiFi 相机开启"访问验证"时需带 SSH 指纹与凭据（官方 sample_connect 流程）
bool CameraController::finishConnect(const SCRSDK::ICrCameraObjectInfo* info, const std::string& label,
                                     const std::string& sshUser, const std::string& sshPass) {
    if(m_impl->m_deviceHandle) disconnectCamera();
    m_impl->lastError=0;
    Logger::info("Connecting to camera: {}", label);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_connecting = true;
    }

    // SSH 认证：先取指纹，再随 Connect 传入凭据
    char fpBuff[128] = { 0 };
    CrInt32u fpLen = 0;
    if (!sshUser.empty() && info->GetSSHsupport() == SCRSDK::CrSSHsupport_ON) {
        if (SCRSDK::GetFingerprint(const_cast<SCRSDK::ICrCameraObjectInfo*>(info), fpBuff, &fpLen)
                != SCRSDK::CrError_None) {
            Logger::warn("GetFingerprint failed - continuing without fingerprint");
            fpLen = 0;
        } else {
            Logger::info("SSH fingerprint: {}", fpBuff);
        }
    }

    SCRSDK::CrDeviceHandle handle = 0;
    SCRSDK::CrError err = SCRSDK::Connect(
        const_cast<SCRSDK::ICrCameraObjectInfo*>(info),
        m_impl.get(), &handle, SCRSDK::CrSdkControlMode_Remote, SCRSDK::CrReconnecting_ON,
        sshUser.c_str(), sshPass.c_str(), fpBuff, fpLen);

    // 0x0002xxxx 区间是 CrWarning（如 Connect_OverLimitOfDevice/Reconnecting）——
    // 警告不代表失败，会话可能已建立，继续等 OnConnected 回调确认；
    // 若判定失败，超时分支会 Disconnect+ReleaseDevice 清理会话，避免相机侧僵尸会话
    bool isWarning = (static_cast<CrInt32u>(err) & 0xFFFF0000u) == 0x00020000u;
    if (err != SCRSDK::CrError_None && !isWarning) {
        m_impl->lastError=static_cast<uint32_t>(err);
        Logger::error("Connect failed: 0x{:x}", err);
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_connecting = false;
        return false;
    }
    if (isWarning) {
        Logger::info("Connect 返回警告 0x{:x}（继续等待 OnConnected 确认）", static_cast<CrInt32u>(err));
    }
    m_impl->m_deviceHandle = handle;
    bool connectedViaWarning = isWarning;

    // Wait for the async OnConnected callback (up to 10 seconds)
    bool connected = false;
    {
        std::unique_lock<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_cv.wait_for(lock, std::chrono::seconds(30), [&] {
            return m_impl->m_connected || (!m_impl->m_connecting && !m_impl->m_connected);
        });
        connected = m_impl->m_connected;
        if (!connected) m_impl->m_connecting = false;
    }

    if (!connected) {
        if(!m_impl->lastError) m_impl->lastError=WAIT_TIMEOUT;
        Logger::error("Connection not established (SDK error or 30s callback timeout)");
        if (connectedViaWarning) {
            // 警告连接（如 OverLimitOfDevice）超时：会话未建立，返回的句柄无效/错位——
            // 实测对它调 Disconnect 会错杀其他相机的会话，因此只复位状态不触碰句柄
            std::lock_guard<std::mutex> lock(m_impl->m_mutex);
            m_impl->m_deviceHandle = 0;
            Logger::warn("警告连接超时，跳过句柄清理（避免误断其他相机会话）");
        } else {
            SCRSDK::Disconnect(m_impl->m_deviceHandle);
            SCRSDK::ReleaseDevice(m_impl->m_deviceHandle);
            m_impl->m_deviceHandle = 0;
        }
        return false;
    }

    m_impl->lastError=0;
    {std::lock_guard<std::mutex> lock(m_impl->m_mutex);
     m_impl->connectedInfo.model=wideToUtf8(info->GetModel());
     m_impl->connectedInfo.name=wideToUtf8(info->GetName());
     m_impl->connectedInfo.cameraObject=nullptr;}
    Logger::info("Camera connection established");
    return true;
}

// 支持的型号列表（顺序 = CrCameraDeviceModelList 枚举值）
const std::vector<std::string>& CameraController::modelNames() {
    static const std::vector<std::string> names = {
        "ILCE-7RM4", "ILCE-9M2", "ILCE-7C", "ILCE-7SM3", "ILCE-1",
        "ILCE-7RM4A", "DSC-RX0M2", "ILCE-7M4", "ILME-FX3", "ILME-FX30",
        "ILME-FX6", "ILCE-7RM5", "ZV-E1", "ILCE-6700", "ILCE-7CM2",
        "ILCE-7CR", "ILX-LR1", "MPC-2610", "ILCE-9M3", "ZV-E10M2",
        "PXW-Z200", "HXR-NX800", "ILCE-1M2", "ILME-FX3A", "BRC-AM7",
        "ILME-FR7", "ILME-FX2", "DSC-RX1RM3", "ILCE-7M5", "PXW-Z300",
        "PXW-Z380", "ILCE-7RM6",
    };
    return names;
}

bool CameraController::disconnectCamera() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    if (!m_impl->m_deviceHandle && !m_impl->m_connected && !m_impl->m_connecting) {
        return true;
    }

    Logger::info("Disconnecting camera");
    SCRSDK::CrError err = SCRSDK::Disconnect(m_impl->m_deviceHandle);
    if (err != SCRSDK::CrError_None) {
        Logger::warn("Disconnect returned 0x{:x}", err);
    }
    SCRSDK::ReleaseDevice(m_impl->m_deviceHandle);
    m_impl->m_deviceHandle = 0;
    // 同步清空缓存的设备对象（DLL 卸载前必须释放，见析构说明）
    // Keep the paired discovery/manual object until controller cleanup or replacement.

    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_connected = false;
    m_impl->m_connecting = false;
    m_impl->m_recording = false;
    m_impl->recordingKnown=false;
    m_impl->m_connectedIp.clear();
    return true;
}

bool CameraController::isConnected() const {
    return m_impl->m_connected.load();
}

std::string CameraController::connectedIp() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_connectedIp;
}

// 按 CrDataType 计算单个可取值在 values 字节数组中的宽度
static int valueEntrySize(SCRSDK::CrDataType type) {
    int base = static_cast<int>(type) & 0x0FFF;
    switch (base) {
        case SCRSDK::CrDataType_Int8:
        case SCRSDK::CrDataType_UInt8: return 1;
        case SCRSDK::CrDataType_Int16:
        case SCRSDK::CrDataType_UInt16: return 2;
        case SCRSDK::CrDataType_UInt32: return 4;
        case SCRSDK::CrDataType_Int64:
        case SCRSDK::CrDataType_UInt64: return 8;
        case SCRSDK::CrDataType_Int32: return 4;
        default: return 0;
    }
}

// 从 values 字节数组解析出可取值列表（小端整数，逐项拼接）
static std::vector<int64_t> parsePossibleValues(const SCRSDK::CrDeviceProperty& p) {
    std::vector<int64_t> out;
    CrInt8u* raw = p.IsSetEnableCurrentValue() ? p.GetSetValues() : p.GetValues();
    CrInt32u bytes = p.IsSetEnableCurrentValue() ? p.GetSetValueSize() : p.GetValueSize();
    if (!raw || bytes == 0) return out;

    int entry = valueEntrySize(p.GetValueType());
    if(!entry || bytes%entry) return out;
    int count = bytes / entry;
    for (int i = 0; i < count; ++i) {
        int base = static_cast<int>(p.GetValueType()) & 0x0FFF;
        bool signedValue=base==SCRSDK::CrDataType_Int8 || base==SCRSDK::CrDataType_Int16 || base==SCRSDK::CrDataType_Int32 || base==SCRSDK::CrDataType_Int64;
        int64_t v=decodeInteger(raw+i*entry,entry,signedValue);
        out.push_back(v);
    }
    return out;
}

static int64_t currentInteger(const SCRSDK::CrDeviceProperty& p) {
    auto value=p.GetCurrentValue(); int width=valueEntrySize(p.GetValueType());
    if(!width) return 0;
    int base=static_cast<int>(p.GetValueType())&0x0fff;
    bool isSigned=base==SCRSDK::CrDataType_Int8 || base==SCRSDK::CrDataType_Int16 || base==SCRSDK::CrDataType_Int32 || base==SCRSDK::CrDataType_Int64;
    return decodeInteger(reinterpret_cast<const uint8_t*>(&value),width,isSigned);
}

std::unordered_map<uint32_t, CameraProperty> CameraController::getCameraProperties() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    std::unordered_map<uint32_t, CameraProperty> properties;
    if (!m_impl->m_connected) return properties;

    SCRSDK::CrDeviceProperty* propList = nullptr;
    CrInt32 numOf = 0;
    SCRSDK::CrError err = SCRSDK::GetDeviceProperties(m_impl->m_deviceHandle, &propList, &numOf);
    if (err != SCRSDK::CrError_None || !propList) {
        Logger::error("GetDeviceProperties failed: 0x{:x}", err);
        return properties;
    }

    for (CrInt32 i = 0; i < numOf; ++i) {
        const SCRSDK::CrDeviceProperty& p = propList[i];
        CameraProperty prop;
        prop.code = p.GetCode();
        prop.readable = p.IsGetEnableCurrentValue();
        prop.writable = p.IsSetEnableCurrentValue();
        prop.currentValue = currentInteger(p);
        prop.valueType=(static_cast<int>(p.GetValueType())&0x0fff)==SCRSDK::CrDataType_UInt64?"UInt64":"Integer";
        prop.range=(static_cast<int>(p.GetValueType()) & SCRSDK::CrDataType_RangeBit)!=0;
        if(!valueEntrySize(p.GetValueType())) prop.readable=prop.writable=false;
        prop.possibleValues = parsePossibleValues(p);
        if (p.GetCode() == SCRSDK::CrDeviceProperty_RecordingState) {
            m_impl->m_recording = prop.currentValue == SCRSDK::CrMovie_Recording_State_Recording; m_impl->recordingKnown = prop.readable;
        }
        properties[prop.code] = prop;
    }

    SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle, propList);
    Logger::debug("Retrieved {} camera properties", properties.size());
    return properties;
}

bool CameraController::setProperty(uint32_t code, int64_t value) {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    Logger::debug("Setting property 0x{:x} to {}", code, value);
    if (!m_impl->m_connected) {
        Logger::error("Cannot set property: not connected");
        return false;
    }

    // Fetch the current property to preserve its value type & size
    CrInt32u codes[1] = { code };
    SCRSDK::CrDeviceProperty* props = nullptr;
    CrInt32 numOf = 0;
    SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(m_impl->m_deviceHandle, 1, codes, &props, &numOf);
    if (err != SCRSDK::CrError_None || numOf != 1) {
        Logger::error("Failed to read property 0x{:x} before setting: 0x{:x}", code, err);
        return false;
    }

    if (!props[0].IsSetEnableCurrentValue() || !valueEntrySize(props[0].GetValueType())) {
        SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle, props); return false;
    }
    int base=static_cast<int>(props[0].GetValueType())&0x0fff;
    bool signedValue=base==SCRSDK::CrDataType_Int8 || base==SCRSDK::CrDataType_Int16 || base==SCRSDK::CrDataType_Int32 || base==SCRSDK::CrDataType_Int64;
    if(!fitsInteger(value,valueEntrySize(props[0].GetValueType()),signedValue)) {
        SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle,props);return false;
    }
    auto allowed=parsePossibleValues(props[0]);
    // SDK range types provide min/max/step; array types list individual settable values.
    bool range=(static_cast<int>(props[0].GetValueType()) & SCRSDK::CrDataType_RangeBit)!=0;
    if(!allowed.empty() && (range ? (allowed.size()<2 || value<allowed[0] || value>allowed[1] || (allowed.size()>2 && allowed[2]>0 && (uint64_t(value)-uint64_t(allowed[0]))%uint64_t(allowed[2])!=0)) : std::find(allowed.begin(),allowed.end(),value)==allowed.end())) {
        SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle,props);return false;
    }
    props[0].SetCurrentValue(value);
    err = SCRSDK::SetDeviceProperty(m_impl->m_deviceHandle, &props[0]);
    SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle, props);

    if (err != SCRSDK::CrError_None) {
        Logger::error("SetDeviceProperty(0x{:x}) failed: 0x{:x}", code, err);
        return false;
    }
    return true;
}

CameraProperty CameraController::getProperty(uint32_t code) {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    CameraProperty prop;
    prop.code = code;
    if (!m_impl->m_connected) return prop;

    CrInt32u codes[1] = { code };
    SCRSDK::CrDeviceProperty* props = nullptr;
    CrInt32 numOf = 0;
    SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(m_impl->m_deviceHandle, 1, codes, &props, &numOf);
    if (err != SCRSDK::CrError_None || numOf != 1) {
        Logger::error("GetProperty(0x{:x}) failed: 0x{:x}", code, err);
        return prop;
    }

    prop.readable = props[0].IsGetEnableCurrentValue();
    prop.writable = props[0].IsSetEnableCurrentValue();
    prop.currentValue = currentInteger(props[0]);
    prop.valueType=(static_cast<int>(props[0].GetValueType())&0x0fff)==SCRSDK::CrDataType_UInt64?"UInt64":"Integer";
    prop.range=(static_cast<int>(props[0].GetValueType()) & SCRSDK::CrDataType_RangeBit)!=0;
    if(!valueEntrySize(props[0].GetValueType())) prop.readable=prop.writable=false;
    prop.possibleValues = parsePossibleValues(props[0]);
    SCRSDK::ReleaseDeviceProperties(m_impl->m_deviceHandle, props);
    return prop;
}

bool CameraController::startRecording() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    Logger::info("Starting recording");
    if (!m_impl->m_connected) return false;

    SCRSDK::CrError err = SCRSDK::SendCommand(m_impl->m_deviceHandle,
        SCRSDK::CrCommandId_MovieRecord, SCRSDK::CrCommandParam_Down);
    if (err != SCRSDK::CrError_None) {
        Logger::error("Start recording failed: 0x{:x}", err);
        return false;
    }
    m_impl->recordingKnown = false;

    auto statusCb = m_impl->m_statusCallback;
    if (statusCb) statusCb(CameraStatus::RECORDING);
    return true;
}

bool CameraController::stopRecording() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    Logger::info("Stopping recording");
    if (!m_impl->m_connected) return false;

    SCRSDK::CrError err = SCRSDK::SendCommand(m_impl->m_deviceHandle,
        SCRSDK::CrCommandId_MovieRecord, SCRSDK::CrCommandParam_Up);
    if (err != SCRSDK::CrError_None) {
        Logger::error("Stop recording failed: 0x{:x}", err);
        return false;
    }
    m_impl->recordingKnown = false;

    auto statusCb = m_impl->m_statusCallback;
    if (statusCb) statusCb(CameraStatus::CONNECTED);
    return true;
}

bool CameraController::isRecording() const {
    return m_impl->m_recording.load();
}

bool CameraController::startLiveView() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    Logger::info("Starting live view");
    if (!m_impl->m_connected) return false;

    // Live view is toggled through the LiveViewStatus device property
    SCRSDK::CrError err = setProperty(SCRSDK::CrDeviceProperty_LiveViewStatus, 1)
        ? SCRSDK::CrError_None : SCRSDK::CrError_Generic_Unknown;
    if (err != SCRSDK::CrError_None) {
        Logger::error("Start live view failed");
        return false;
    }
    return true;
}

bool CameraController::stopLiveView() {
    CameraControllerImpl::ControlGuard operationLock(*m_impl);
    Logger::info("Stopping live view");
    if (!m_impl->m_connected) return false;

    SCRSDK::CrError err = setProperty(SCRSDK::CrDeviceProperty_LiveViewStatus, 0)
        ? SCRSDK::CrError_None : SCRSDK::CrError_Generic_Unknown;
    if (err != SCRSDK::CrError_None) {
        Logger::error("Stop live view failed");
        return false;
    }
    return true;
}

std::vector<uint8_t> CameraController::getLiveViewFrame() {
    if(m_impl->controlWaiting) return {};
    std::unique_lock<std::recursive_mutex> operationLock(m_impl->operationMutex,std::try_to_lock);
    if(!operationLock.owns_lock()) return {};
    if (!m_impl->m_connected) return {};

    // Query the frame buffer size, allocate, then fetch the JPEG frame
    SCRSDK::CrImageInfo imageInfo;
    SCRSDK::CrError err = SCRSDK::GetLiveViewImageInfo(m_impl->m_deviceHandle, &imageInfo);
    if (err != SCRSDK::CrError_None) return {};

    CrInt32u bufSize = imageInfo.GetBufferSize();
    if (bufSize == 0) return {};

    std::vector<uint8_t> buffer(bufSize);
    SCRSDK::CrImageDataBlock block;
    block.SetData(buffer.data());
    block.SetSize(bufSize);

    err = SCRSDK::GetLiveViewImage(m_impl->m_deviceHandle, &block);
    if (err != SCRSDK::CrError_None) return {};

    CrInt32u frameSize = block.GetImageSize();
    if (frameSize == 0 || frameSize > bufSize) return {};

    // 实测帧布局：约 176 字节(0xB0)块头 + JPEG 数据。
    // 重要发现：GetImageSize() 给出的尺寸可能不含 JPEG 尾部（EOI/最后几行 MCU），
    // 完整数据实际延伸到缓冲区更深处——只按 frameSize 截断会导致解码时底部出现灰条。
    // 因此 SOI/EOI 在整个缓冲区内搜索，以 EOI 为准截取完整帧。
    return extractJpeg(buffer);
}

CameraInfo CameraController::getCameraInfo() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->connectedInfo;
}

CameraStatus CameraController::getCameraStatus() const {
    if (!m_impl->m_connected) return CameraStatus::DISCONNECTED;
    if (m_impl->m_recording) return CameraStatus::RECORDING;
    return CameraStatus::CONNECTED;
}

void CameraController::setPropertyChangedCallback(std::function<void(uint32_t, int64_t)> callback) {
    m_impl->m_propertyCallback = callback;
}

void CameraController::setStatusChangedCallback(std::function<void(CameraStatus)> callback) {
    m_impl->m_statusCallback = callback;
}

void CameraController::clearDiscovery() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->operationMutex);
    m_impl->m_discovery.clear(); m_impl->m_lastEnum.reset(); m_impl->m_manualInfo.reset();
}
bool CameraController::recordingKnown() const { return m_impl->m_connected && m_impl->recordingKnown.load(); }

void CameraController::refreshRecording() {
    auto p=getProperty(SCRSDK::CrDeviceProperty_RecordingState);
    m_impl->recordingKnown=p.readable;
    if(p.readable) m_impl->m_recording=p.currentValue==SCRSDK::CrMovie_Recording_State_Recording;
}
CameraStatus CameraController::connectionState() const {
    return m_impl->m_connected ? CameraStatus::CONNECTED : m_impl->m_connecting ? CameraStatus::CONNECTING : CameraStatus::DISCONNECTED;
}
uint32_t CameraController::lastError() const {return m_impl->lastError.load();}
