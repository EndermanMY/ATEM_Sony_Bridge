#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <memory>
#include "nlohmann/json.hpp"
#include "sony/CameraController.h"
#include "atem/AtemController.h"
#include "mapping/MappingEngine.h"
#include "config/ConfigManager.h"
#include "../discovery/NotifyDiscovery.h"

// BridgeService is the application interaction layer: it wires the ATEM
// controller, the Sony camera controller and the mapping engine together,
// owns persistent configuration, and exposes a JSON-oriented API consumed
// by both the Web UI and the interactive console.
class BridgeService {
public:
    BridgeService();
    ~BridgeService();

    // Load config + rules, initialize the Sony SDK and wire callbacks.
    bool startup();
    void shutdown();

    // ---- 后台轮询发现（相机进入遥控配对界面后自动出现在列表）----
    void startPolling();
    void stopPolling();
    void pollLoop();
    nlohmann::json getDiscovered();   // 当前已发现的相机列表

    // ---- status / config ----
    nlohmann::json getStatus();
    nlohmann::json getConfig();
    bool applyConfig(const nlohmann::json& cfg);   // applies + saves
    nlohmann::json getLogs();

    // ---- ATEM ----
    nlohmann::json connectAtem(const std::string& ip, int port);
    nlohmann::json disconnectAtem();

    // ---- Sony camera ----
    nlohmann::json discoverCameras();
    // 同步版发现（添加相机向导用）：CrSDK 枚举，结果同时更新发现列表
    std::vector<CameraInfo> discoverCamerasSync(int timeoutSec);
    // NOTIFY 被动发现的相机端点（IP 形式）
    std::vector<CameraInfo> notifyEndpoints() const;
    const std::vector<CameraInfo>& lastDiscoverySnapshot() const { return m_discovered; }
    // WiFi 相机开启"访问验证"时的 SSH 凭据（UI 输入后设置）
    void setSshCredentials(const std::string& user, const std::string& pass);
    // NOTIFY 发现的相机没有型号信息，连接时使用 UI 下拉框选择的型号
    void setDefaultModelId(int modelId) { m_defaultModelId = modelId; }
    // 暂停后台轮询/自动连接（添加相机向导搜索时独占 SDK 通道）
    void setDiscoveryPaused(bool paused) { m_discoveryPaused = paused; }

    // ---- 已绑定相机库（软件记住的相机，自动连接；删除后不再连接）----
    nlohmann::json addBoundCamera(const std::string& ip, int modelId, const std::string& name,
                                  const std::string& sshUser, const std::string& sshPass, int cameraId);
    nlohmann::json removeBoundCamera(const std::string& ip);
    nlohmann::json getBoundCameras();   // 供外部调用（内部自行加锁）
    bool renameBoundCamera(const std::string& ip, const std::string& newName);
    bool setBoundCameraId(const std::string& ip, int cameraId);   // ATEM 机位号 1-15
    // 相机连接状态（按相机 IP 查询；无参 = 当前选中相机）
    bool isCameraConnected(const std::string& ip = std::string()) const;
    bool isCameraRecording(const std::string& ip = std::string()) const;
    bool isTallySupported(const std::string& ip = std::string()) const;
    // 当前选中相机（详情页监看/录制/属性的操作目标）
    void setActiveCamera(const std::string& ip);
    std::string activeCameraIp() const;

    // ---- 图像回传监看（LiveView 推流）----
    // WebHost 注入事件回调：字符串为完整 JSON（{"event":...}），在工作线程调用，
    // 实现方需自行投递回 UI 线程
    void setEventCallback(std::function<void(const std::string&)> cb) { m_eventCallback = std::move(cb); }
    // 开启/停止监看推流（30fps 上限 JPEG base64；推流目标 = 指定/选中相机）
    void startLiveViewStreams();   // 为当前所有已连接相机各开一路推流
    void stopLiveViewStream();
    bool isLiveViewStreaming() const { return m_lvRunning; }
    // 监看画质设置（分辨率档 + 压缩质量）：持久化；推流中立即下发
    nlohmann::json setLiveviewQuality(const std::string& ip, int imageQuality, int compression);
    nlohmann::json getLiveviewQuality(const std::string& ip = std::string());
    nlohmann::json connectCamera(int discoveryIndex,
                                 const std::string& sshUser = std::string(), const std::string& sshPass = std::string());
    nlohmann::json connectCameraByIp(const std::string& ip, int modelId,
                                     const std::string& sshUser = std::string(), const std::string& sshPass = std::string());
    nlohmann::json disconnectCamera(const std::string& ip = std::string());
    nlohmann::json startRecording(const std::string& ip = std::string());
    nlohmann::json stopRecording(const std::string& ip = std::string());
    nlohmann::json getProperties(const std::string& ip = std::string());   // camera property list
    nlohmann::json setProperty(const std::string& ip, uint32_t code, int64_t value);

    // ---- mapping rules ----
    nlohmann::json getRules();
    bool saveRules(const nlohmann::json& rulesArray);
    bool toggleRule(const std::string& ruleId, bool enabled);
    bool removeRule(const std::string& ruleId);

    // Paths are resolved relative to the executable/config directory.
    static std::string resourcePath(const std::string& relative);

private:
    nlohmann::json getBoundCamerasUnlocked();   // 供已持有 m_mutex 的内部代码调用
    void wireCallbacks();
    void onAtemProgramChanged(int input);
    void onAtemPreviewChanged(int input);
    void onAtemRecordingChanged(bool recording);
    void onAtemTallyChanged(int input, bool state);

    mutable std::recursive_mutex m_mutex;
    mutable std::recursive_mutex m_registryMutex;
    std::vector<std::unique_ptr<CameraController>> m_retired;
    std::unordered_map<std::string, bool> m_manualDisconnected;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_retryAt;
    std::unordered_map<std::string, std::string> m_lastError;
    ConfigManager m_config;
    AtemController m_atem;
    MappingEngine m_mapping;

    // 多相机：每台相机一个控制器实例（IP/USB:序列号 → 控制器），可并行连接
    CameraController* camera(const std::string& ip);              // 取/建（空 ip = 当前选中）
    CameraController* findCamera(const std::string& ip) const;    // 只查不建
    std::string ipOf(const CameraController* c) const;            // 反查 IP（回调定位用）
    std::unordered_map<std::string, std::unique_ptr<CameraController>> m_cameras;
    std::string m_activeIp;                    // UI 当前选中相机（监看/录制/属性目标）
    CameraController m_enumCam;                // 专用枚举器（发现列表与按索引连接共用）
    std::unordered_map<std::string, int> m_aliveFail;      // 每相机在线探测失败计数
    std::unordered_map<std::string, bool> m_tallyByIp;     // 每相机 Tally 支持探测结果
    void refreshMappingCamera();               // 规则引擎指向当前选中相机
    void onCameraStatus(CameraController* cam, CameraStatus status);   // 每控制器的状态回调

    std::atomic<bool> m_started{false};
    std::vector<CameraInfo> m_discovered;
    std::thread m_pollThread;
    std::atomic<bool> m_polling{false};
    std::string m_sshUser;   // SSH 凭据（WiFi 访问验证）
    std::string m_sshPass;
    int m_defaultModelId = 8;   // NOTIFY 发现的相机连接时用的型号（默认 ILME-FX3）
    NotifyDiscovery m_notify;   // 被动发现：路由器组播受限时靠相机 NOTIFY 广播找相机
    int m_autoConnectCooldown = 0;  // 自动连接重试冷却（轮询周期数）
    int m_discoveryCooldown = 0;    // 后台发现枚举冷却（≈60 秒一次，枚举会占用相机会话槽）

    std::function<void(const std::string&)> m_eventCallback;  // UI 事件推送（监看帧等）
    std::thread m_lvThread;                 // 监看推流管理线程（为每台相机分配 worker）
    std::atomic<bool> m_lvRunning{false};   // 推流运行标志
    std::unordered_map<std::string, std::thread> m_lvWorkers;  // 每相机独立推流线程（并行取帧，消除串行等待）
    void liveViewLoop();                    // 管理循环：动态为连接中的相机开/收 worker
    void liveViewWorker(const std::string& ip);  // 单相机推流循环（独立线程，≈15fps/路）
    std::atomic<bool> m_discoveryPaused{false};  // 向导搜索期间暂停后台竞争
    std::mutex m_sdkMutex;           // 串行化 SDK 枚举/连接调用（耗时长，勿在 UI 线程等待）
    std::atomic<bool> m_sdkBusy{false};  // 用户 SDK 操作进行中（轮询暂停，防止饿死）
    mutable std::mutex m_discoveredMutex;  // 轻量锁：仅保护 m_discovered 快照（UI 线程读取用）

};
