#include "AtemController.h"
#include "common/Logger.h"
#include <mutex>
#include <thread>
#include <chrono>


class AtemControllerImpl {
public:
    bool m_connected=false;
    std::string m_ipAddress;
    int m_port=9910;
    int m_programInput=0;
    int m_previewInput=0;
    bool m_recording=false;
    std::function<void(int)> m_programCallback;
    std::function<void(int)> m_previewCallback;
    std::function<void(bool)> m_recordingCallback;
    std::function<void(int, bool)> m_tallyCallback;
};

AtemController::AtemController() : m_impl(new AtemControllerImpl()) {
}

AtemController::~AtemController() {
}

bool AtemController::connect(const AtemConnectionInfo&) {
    Logger::warn("ATEM integration not implemented (planned for 0.3.0)");return false;
}

bool AtemController::disconnect() {
    m_impl->m_connected=false;
    return true;
}

bool AtemController::isConnected() const {
    return m_impl->m_connected;
}

bool AtemController::sendCommand(const std::string& command) {
    (void)command;
    return false; // Future official SDK adapter boundary.
}

bool AtemController::sendCommand(AtemCommand, const std::vector<std::string>&) {
    return false;
}

AtemInput AtemController::getProgramInput() const {
    AtemInput input;
    input.index = m_impl->m_programInput;
    input.isProgram = true;
    return input;
}

AtemInput AtemController::getPreviewInput() const {
    AtemInput input;
    input.index = m_impl->m_previewInput;
    input.isPreview = true;
    return input;
}

std::vector<AtemInput> AtemController::getAllInputs() const {
    std::vector<AtemInput> inputs;
    // TODO: Query actual ATEM inputs
    return inputs;
}

AtemRecordingStatus AtemController::getRecordingStatus() const {
    AtemRecordingStatus status;
    status.isRecording = m_impl->m_recording;
    return status;
}

void AtemController::setProgramChangedCallback(std::function<void(int)> callback) {
    m_impl->m_programCallback = callback;
}

void AtemController::setPreviewChangedCallback(std::function<void(int)> callback) {
    m_impl->m_previewCallback = callback;
}

void AtemController::setRecordingStatusCallback(std::function<void(bool)> callback) {
    m_impl->m_recordingCallback = callback;
}

void AtemController::setTallyChangedCallback(std::function<void(int, bool)> callback) {
    m_impl->m_tallyCallback = callback;
}

AtemCommand AtemController::parseCommand(const std::string& command) {
    if (command.rfind("PrgI ", 0) == 0) return AtemCommand::PROGRAM_CHANGE;
    if (command.rfind("PrvI ", 0) == 0) return AtemCommand::PREVIEW_CHANGE;
    if (command.rfind("Recd 1", 0) == 0) return AtemCommand::RECORD_START;
    if (command.rfind("Recd 0", 0) == 0) return AtemCommand::RECORD_STOP;
    if (command.rfind("TrSS ", 0) == 0) return AtemCommand::TALLY_CHANGE;
    return AtemCommand::UNKNOWN;
}

std::vector<std::string> AtemController::parseParameters(const std::string& command) {
    std::vector<std::string> params;
    size_t spacePos = command.find(' ');
    if (spacePos == std::string::npos) {
        return params;
    }
    
    std::string paramStr = command.substr(spacePos + 1);
    size_t start = 0;
    size_t end = 0;
    
    while ((end = paramStr.find(' ', start)) != std::string::npos) {
        params.push_back(paramStr.substr(start, end - start));
        start = end + 1;
    }
    
    if (start < paramStr.length()) {
        params.push_back(paramStr.substr(start));
    }
    
    return params;
}
