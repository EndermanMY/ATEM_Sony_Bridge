#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

// ATEM connection info
struct AtemConnectionInfo {
    std::string ipAddress = "192.168.1.240"; // Default ATEM IP
    int port = 9910;                         // Default ATEM port
    std::string model;                       // ATEM model (e.g., "ATEM Mini Pro")
    int numInputs = 4;                       // Number of inputs
};

// ATEM input status
struct AtemInput {
    int index;               // Input number (0-based)
    std::string name;        // Input name
    bool isProgram = false;  // Currently on program
    bool isPreview = false;  // Currently on preview
    bool tally = false;      // Tally light status
};

// ATEM recording/streaming status
struct AtemRecordingStatus {
    bool isRecording = false;
    bool isStreaming = false;
    std::string recordingTime;
    std::string streamingTime;
};

// ATEM command types (from ATEM Software Control protocol)
enum class AtemCommand {
    UNKNOWN,
    PROGRAM_CHANGE,      // PrgI <index>
    PREVIEW_CHANGE,      // PrvI <index>
    TRANSITION,          // TrSS <type> <rate>
    RECORD_START,        // Recd 1
    RECORD_STOP,         // Recd 0
    STREAM_START,        // StrR 1
    STREAM_STOP,         // StrR 0
    TALLY_CHANGE,        // TrSS <input> <state>
    AUDIO_LEVEL,         // AuLv <input> <level>
    AUX_CHANGE,          // AuxS <aux> <input>
    MACRO_RUN,           // MACR <index>
    KEY_ON,              // KeOn <key>
    KEY_OFF,             // KeOf <key>
};

class AtemController {
public:
    AtemController();
    ~AtemController();
    
    // Connection management
    bool connect(const AtemConnectionInfo& info);
    bool disconnect();
    bool isConnected() const;
    
    // Command sending
    bool sendCommand(const std::string& command);
    bool sendCommand(AtemCommand command, const std::vector<std::string>& params = {});
    
    // Status queries
    AtemInput getProgramInput() const;
    AtemInput getPreviewInput() const;
    std::vector<AtemInput> getAllInputs() const;
    AtemRecordingStatus getRecordingStatus() const;
    
    // Event callbacks
    void setProgramChangedCallback(std::function<void(int)> callback);
    void setPreviewChangedCallback(std::function<void(int)> callback);
    void setRecordingStatusCallback(std::function<void(bool)> callback);
    void setTallyChangedCallback(std::function<void(int, bool)> callback);
    
    // Protocol parsing
    static AtemCommand parseCommand(const std::string& command);
    static std::vector<std::string> parseParameters(const std::string& command);

private:
    std::unique_ptr<class AtemControllerImpl> m_impl;
};
