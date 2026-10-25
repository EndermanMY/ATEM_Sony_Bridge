#pragma once

#include <string>
#include <vector>
#include <unordered_map>

// Application configuration
struct AppConfig {
    // ATEM settings
    struct AtemConfig {
        std::string ipAddress = "192.168.1.240";
        int port = 9910;
        bool autoConnect = false;
    } atem;
    
    // Camera settings
    struct CameraConfig {
        std::vector<std::string> knownCameras;
        bool autoDiscover = true;
        int discoveryTimeout = 3; // seconds
    } camera;
    
    // UI settings
    struct UiConfig {
        bool showLiveView = true;
        bool showTallyLights = true;
        bool darkMode = true;
        int refreshRate = 30; // Hz
    } ui;
    
    // 监看设置（LiveView 画质）
    struct LiveviewConfig {
        int imageQuality = 1;   // 分辨率档位：0=Low(640px 宽) 1=High(1024px 宽)
        int compression = -1;   // 压缩质量数值（min~max，越高越清晰）；-1=开启推流时自动取最高档
    } liveview;

    // Logging settings
    struct LogConfig {
        std::string level = "info";
        std::string filePath = "atem_sony_bridge.log";
        bool consoleOutput = true;
    } log;
    
    // Mapping settings
    struct MappingConfig {
        std::string rulesFile = "mapping_rules.json";
        bool autoLoadRules = true;
    } mapping;

    // 已绑定相机（软件记住的相机，自动连接；删除后不再连接）
    struct BoundCamera {
        std::string ip;
        std::string name;      // 用户自定义显示名
        int modelId = 8;       // CrCameraDeviceModelList 枚举值
        int cameraId = 0;      // ATEM 机位号 1-15（0=未分配），M5 指令映射用
        int imageQuality = -1;
        int compression = -2;
        std::string sshUser;
        std::string sshPass;
    };
    std::vector<BoundCamera> cameras;
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();
    
    // Configuration management
    bool loadConfig(const std::string& filename = "config.json");
    bool saveConfig(const std::string& filename = "config.json");
    bool commitConfig(const AppConfig& config, const std::string& filename);
    void resetToDefaults();
    bool migrationNeeded() const { return m_migrationNeeded; }
    
    // Getters and setters
    AppConfig getConfig() const { return m_config; }
    void setConfig(const AppConfig& config) { m_config = config; }
    
    // Individual getters
    std::string getAtemIp() const { return m_config.atem.ipAddress; }
    int getAtemPort() const { return m_config.atem.port; }
    std::string getLogLevel() const { return m_config.log.level; }
    std::string getRulesFile() const { return m_config.mapping.rulesFile; }
    
    // Individual setters
    void setAtemIp(const std::string& ip) { m_config.atem.ipAddress = ip; }
    void setAtemPort(int port) { m_config.atem.port = port; }
    void setLogLevel(const std::string& level) { m_config.log.level = level; }
    void setRulesFile(const std::string& file) { m_config.mapping.rulesFile = file; }

private:
    bool m_migrationNeeded=false;
    AppConfig m_config;
    std::string m_configPath;
};
