#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include "atem/AtemController.h"

// Mapping rule types
enum class MappingType {
    DIRECT,      // One-to-one mapping: ATEM command -> Sony command
    CONDITIONAL, // Mapping with conditions
    SEQUENCE,    // Multiple Sony commands for one ATEM command
    TIMED,       // Delayed or timed sequence
};

// Mapping condition
struct MappingCondition {
    std::string type;           // "atem_input", "camera_state", "time", etc.
    std::string parameter;      // Condition parameter
    std::string value;          // Expected value
    std::string comparison;     // "equals", "not_equals", "greater", "less"
};

// Mapping action
struct MappingAction {
    std::string command;        // Sony command: start_recording, stop_recording,
                                // start_liveview, stop_liveview, set_property
    std::string parameter;      // for set_property: property code (hex/dec string)
    std::string value;          // Value to set
    int delayMs = 0;            // Delay before execution
};

// Mapping rule
struct MappingRule {
    std::string id;
    std::string name;
    MappingType type = MappingType::DIRECT;
    AtemCommand atemCommand = AtemCommand::UNKNOWN;    // Trigger ATEM command
    std::vector<MappingCondition> conditions;
    std::vector<MappingAction> actions;
    bool enabled = true;
};

class MappingEngine {
public:
    MappingEngine();
    ~MappingEngine();
    
    // Rule management
    bool loadRulesFromFile(const std::string& filename);
    bool saveRulesToFile(const std::string& filename);
    
    void addRule(const MappingRule& rule);
    void removeRule(const std::string& ruleId);
    void updateRule(const MappingRule& rule);
    std::vector<MappingRule> getRules() const;
    MappingRule getRule(const std::string& ruleId) const;
    
    // Mapping execution
    bool processAtemCommand(AtemCommand command, const std::vector<std::string>& params, class CameraController* target);
    bool processAtemCommand(const std::string& rawCommand, class CameraController* target);
    
    // Rule matching
    std::vector<MappingRule> findMatchingRules(AtemCommand command, const std::vector<std::string>& params) const;
    
    // Configuration
    void setAtemController(AtemController* controller) { m_atemController = controller; }
    
private:
    class MappingEngineImpl* m_impl;
    AtemController* m_atemController = nullptr;
};
