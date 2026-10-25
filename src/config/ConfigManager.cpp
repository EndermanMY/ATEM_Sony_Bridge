#include "ConfigManager.h"
#include "common/Logger.h"
#include "common/Validation.h"
#include "common/AtomicFile.h"
#include <set>
#include <fstream>
#include <filesystem>
#include <windows.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ConfigManager::ConfigManager() {
    resetToDefaults();
}

ConfigManager::~ConfigManager() {
}

bool ConfigManager::loadConfig(const std::string& filename) {
    const auto previous = m_config;
    const bool previousMigration=m_migrationNeeded;
    m_configPath = filename;
    
    try {
        std::ifstream file(std::filesystem::u8path(filename));
        if (!file.is_open()) {
            Logger::warn("Config file not found: {}, using defaults", filename);
            return false;
        }
        
        json j;
        file >> j;
        m_config=AppConfig{};
        m_migrationNeeded=j.value("schemaVersion",0)<1;
        
        // Parse ATEM config
        if (j.contains("atem")) {
            auto& atem = j["atem"];
            if (atem.contains("ipAddress")) m_config.atem.ipAddress = atem["ipAddress"];
            if (atem.contains("port")) m_config.atem.port = atem["port"];
            if (atem.contains("autoConnect")) m_config.atem.autoConnect = atem["autoConnect"];
        }
        
        // Parse camera config
        if (j.contains("camera")) {
            auto& camera = j["camera"];
            if (camera.contains("autoDiscover")) m_config.camera.autoDiscover = camera["autoDiscover"];
            if (camera.contains("discoveryTimeout")) m_config.camera.discoveryTimeout = camera["discoveryTimeout"];
            if (camera.contains("knownCameras") && camera["knownCameras"].is_array()) {
                m_config.camera.knownCameras.clear();
                for (const auto& cam : camera["knownCameras"]) {
                    m_config.camera.knownCameras.push_back(cam);
                }
            }
        }
        
        // Parse UI config
        if (j.contains("ui")) {
            auto& ui = j["ui"];
            if (ui.contains("showLiveView")) m_config.ui.showLiveView = ui["showLiveView"];
            if (ui.contains("showTallyLights")) m_config.ui.showTallyLights = ui["showTallyLights"];
            if (ui.contains("darkMode")) m_config.ui.darkMode = ui["darkMode"];
            if (ui.contains("refreshRate")) m_config.ui.refreshRate = ui["refreshRate"];
        }
        
        // Parse log config
        if (j.contains("log")) {
            auto& log = j["log"];
            if (log.contains("level")) m_config.log.level = log["level"];
            if (log.contains("filePath")) m_config.log.filePath = log["filePath"];
            if (log.contains("consoleOutput")) m_config.log.consoleOutput = log["consoleOutput"];
        }
        
        // Parse mapping config
        if (j.contains("mapping")) {
            auto& mapping = j["mapping"];
            if (mapping.contains("rulesFile")) m_config.mapping.rulesFile = mapping["rulesFile"];
            if (mapping.contains("autoLoadRules")) m_config.mapping.autoLoadRules = mapping["autoLoadRules"];
        }

        // Parse liveview config
        if (j.contains("liveview")) {
            auto& lv = j["liveview"];
            if (lv.contains("imageQuality")) m_config.liveview.imageQuality = lv["imageQuality"];
            if (lv.contains("compression")) m_config.liveview.compression = lv["compression"];
        }

        // Parse bound camera list
        if (j.contains("cameras") && j["cameras"].is_array()) {
            m_config.cameras.clear();
            for (const auto& cam : j["cameras"]) {
                AppConfig::BoundCamera bc;
                bc.ip = cam.value("ip", std::string());
                bc.name = cam.value("name", std::string());
                bc.modelId = cam.value("modelId", 8);
                bc.cameraId = cam.value("cameraId", 0);
                bc.imageQuality = cam.value("imageQuality", -1);
                bc.compression = cam.value("compression", -2);
                bc.sshUser = cam.value("sshUser", std::string());
                bc.sshPass = cam.value("sshPass", std::string());
                if (!bc.ip.empty()) m_config.cameras.push_back(bc);
            }
        }
        
        std::set<std::string> ips; std::set<int> ids;
        for(const auto& c:m_config.cameras) {
            if((!validIpv4(c.ip) && !(c.ip.rfind("USB:",0)==0 && c.ip.size()>4)) || !ips.insert(c.ip).second || c.modelId<0 || c.modelId>=32 || c.cameraId<0 || c.cameraId>15 || (c.cameraId && !ids.insert(c.cameraId).second))
                throw std::invalid_argument("Invalid or duplicate camera binding/ID");
        }
        for(auto& c:m_config.cameras) if(!c.cameraId) {c.cameraId=allocateCameraId(m_config.cameras,c.ip,0);m_migrationNeeded=true;}
        Logger::info("Configuration loaded from {}", filename);
        return true;
        
    } catch (const std::exception& e) {
        m_config = previous;
        m_migrationNeeded=previousMigration;
        Logger::error("Failed to load config: {}", e.what());
        return false;
    }
}

bool ConfigManager::saveConfig(const std::string& filename) {
    std::string path = filename.empty() ? m_configPath : filename;
    
    try {
        json j;
        j["schemaVersion"] = 1;
        
        // ATEM config
        j["atem"]["ipAddress"] = m_config.atem.ipAddress;
        j["atem"]["port"] = m_config.atem.port;
        j["atem"]["autoConnect"] = m_config.atem.autoConnect;
        
        // Camera config
        j["camera"]["autoDiscover"] = m_config.camera.autoDiscover;
        j["camera"]["discoveryTimeout"] = m_config.camera.discoveryTimeout;
        j["camera"]["knownCameras"] = m_config.camera.knownCameras;
        
        // UI config
        j["ui"]["showLiveView"] = m_config.ui.showLiveView;
        j["ui"]["showTallyLights"] = m_config.ui.showTallyLights;
        j["ui"]["darkMode"] = m_config.ui.darkMode;
        j["ui"]["refreshRate"] = m_config.ui.refreshRate;
        
        // Log config
        j["log"]["level"] = m_config.log.level;
        j["log"]["filePath"] = m_config.log.filePath;
        j["log"]["consoleOutput"] = m_config.log.consoleOutput;
        
        // Mapping config
        j["mapping"]["rulesFile"] = m_config.mapping.rulesFile;
        j["mapping"]["autoLoadRules"] = m_config.mapping.autoLoadRules;

        // Liveview config
        j["liveview"]["imageQuality"] = m_config.liveview.imageQuality;
        j["liveview"]["compression"] = m_config.liveview.compression;

        // Bound camera list
        nlohmann::json cams = nlohmann::json::array();
        for (const auto& bc : m_config.cameras) {
            cams.push_back({
                {"ip", bc.ip}, {"name", bc.name}, {"modelId", bc.modelId},
                {"cameraId", bc.cameraId}, {"imageQuality", bc.imageQuality}, {"compression", bc.compression},
                {"sshUser", bc.sshUser}, {"sshPass", bc.sshPass},
            });
        }
        j["cameras"] = cams;
        
        writeAtomicText(path,j.dump(2));

        m_migrationNeeded=false;
        Logger::info("Configuration saved to {}", path);
        return true;
        
    } catch (const std::exception& e) {
        Logger::error("Failed to save config: {}", e.what());
        return false;
    }
}

bool ConfigManager::commitConfig(const AppConfig& config, const std::string& filename) {
    const auto previous=m_config;
    m_config=config;
    if(saveConfig(filename)) return true;
    m_config=previous;
    return false;
}

void ConfigManager::resetToDefaults() {
    m_config = AppConfig{}; // Reset to default values
    Logger::info("Configuration reset to defaults");
}
