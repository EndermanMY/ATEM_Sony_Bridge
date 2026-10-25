#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include "CameraRemote_SDK.h"
#include "CrDeviceProperty.h"
#include "IDeviceCallback.h"

// Forward declarations
class CameraControllerImpl;

// Camera connection information
struct CameraInfo {
    std::string name;           // UTF-8
    std::string model;          // UTF-8
    std::string serialNumber;   // UTF-8 (device id)
    std::string connectionType; // "USB" or "Ethernet"
    std::string ipAddress;      // For Ethernet connections
    bool pairingRequired = false;
    const SCRSDK::ICrCameraObjectInfo* cameraObject = nullptr; // valid while discovery list alive
};

// Camera property value
struct CameraProperty {
    uint32_t code = 0;
    std::string name;
    bool readable = false;
    bool writable = false;
    bool range = false;
    int64_t currentValue = 0;
    int64_t defaultValue = 0;
    std::vector<int64_t> possibleValues;
    std::string valueType; // "UInt8", "Int8", "UInt16", ...
};

// Camera status (ERR 避免与 windows.h 的 ERROR 宏冲突)
enum class CameraStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECORDING,
    STREAMING,
    ERR
};

class CameraController {
public:
    CameraController();
    ~CameraController();

    // SDK Initialization
    static bool initializeSDK();
    static void releaseSDK();
    static bool isSDKInitialized();

    // Camera discovery (real SDK enumeration). The returned list stays
    // valid until the next discoverCameras() call.
    std::vector<CameraInfo> discoverCameras(int timeoutSec = 3);
    std::vector<CameraInfo> lastDiscovery() const;

    // Connection management. SSH 参数用于"访问验证开启"的 WiFi 相机
    // （GetFingerprint + Connect 带 userId/password，官方文档标准流程）
    bool connectToCamera(const CameraInfo& camera,
                         const std::string& sshUser = "", const std::string& sshPass = "");
    bool connectToCamera(int discoveryIndex,
                         const std::string& sshUser = "", const std::string& sshPass = "");
    bool connectByIp(const std::string& ip, int modelId,
                     const std::string& sshUser = "", const std::string& sshPass = "");
    bool disconnectCamera();
    bool isConnected() const;
    // 当前连接相机的 IP（USB 连接返回空；用于后台在线探测）
    std::string connectedIp() const;

    // Supported camera models (index = CrCameraDeviceModelList enum value)
    static const std::vector<std::string>& modelNames();

    // Property management
    std::unordered_map<uint32_t, CameraProperty> getCameraProperties();
    bool setProperty(uint32_t code, int64_t value);
    void clearDiscovery();
    bool recordingKnown() const;
    void refreshRecording();
    CameraStatus connectionState() const;
    uint32_t lastError() const;
    CameraProperty getProperty(uint32_t code);

    // Recording control
    bool startRecording();
    bool stopRecording();
    bool isRecording() const;

    // Live view
    bool startLiveView();
    bool stopLiveView();
    std::vector<uint8_t> getLiveViewFrame();

    // Camera information
    CameraInfo getCameraInfo() const;
    CameraStatus getCameraStatus() const;

    // Event handling
    void setPropertyChangedCallback(std::function<void(uint32_t, int64_t)> callback);
    void setStatusChangedCallback(std::function<void(CameraStatus)> callback);

private:
    // 从设备对象发起连接并等待回调（connectToCamera / connectByIp 共用）
    bool finishConnect(const SCRSDK::ICrCameraObjectInfo* info, const std::string& label,
                       const std::string& sshUser, const std::string& sshPass);

    std::unique_ptr<CameraControllerImpl> m_impl;
};
