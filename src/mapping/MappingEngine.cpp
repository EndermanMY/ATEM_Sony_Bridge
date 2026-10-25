#include "MappingEngine.h"
#include "MappingPolicy.h"
#include "common/AtomicFile.h"
#include "sony/CameraController.h"
#include "common/Logger.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class MappingEngineImpl {
public:
    std::vector<MappingRule> rules;
    std::unordered_map<std::string, MappingRule> ruleMap;
};

MappingEngine::MappingEngine() : m_impl(new MappingEngineImpl()) {
}

MappingEngine::~MappingEngine() { delete m_impl; }

bool MappingEngine::loadRulesFromFile(const std::string& filename) {
    Logger::info("Loading mapping rules from: {}", filename);

    try {
        std::ifstream file(std::filesystem::u8path(filename));
        if (!file.is_open()) {
            Logger::warn("Mapping rules file not found (skipping): {}", filename);
            return false;
        }

        json j;
        file >> j;

        if (!j.is_array()) {
            Logger::error("Mapping rules root must be an array");
            return false;
        }

        m_impl->rules.clear();
        m_impl->ruleMap.clear();

        for (const auto& rj : j) {
            MappingRule rule;
            rule.id = rj.value("id", std::string("rule-") + std::to_string(m_impl->rules.size()));
            rule.name = rj.value("name", std::string("unnamed"));
            rule.enabled = rj.value("enabled", true);

            std::string type = rj.value("type", std::string("DIRECT"));
            if (type == "CONDITIONAL") rule.type = MappingType::CONDITIONAL;
            else if (type == "SEQUENCE") rule.type = MappingType::SEQUENCE;
            else if (type == "TIMED") rule.type = MappingType::TIMED;
            else rule.type = MappingType::DIRECT;

            std::string cmd = rj.value("atemCommand", std::string("UNKNOWN"));
            if (cmd == "PROGRAM_CHANGE") rule.atemCommand = AtemCommand::PROGRAM_CHANGE;
            else if (cmd == "PREVIEW_CHANGE") rule.atemCommand = AtemCommand::PREVIEW_CHANGE;
            else if (cmd == "RECORD_START") rule.atemCommand = AtemCommand::RECORD_START;
            else if (cmd == "RECORD_STOP") rule.atemCommand = AtemCommand::RECORD_STOP;
            else if (cmd == "TALLY_CHANGE") rule.atemCommand = AtemCommand::TALLY_CHANGE;
            else rule.atemCommand = AtemCommand::UNKNOWN;

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

            m_impl->rules.push_back(rule);
            m_impl->ruleMap[rule.id] = rule;
        }

        Logger::info("Loaded {} mapping rules", m_impl->rules.size());
        return true;

    } catch (const std::exception& e) {
        Logger::error("Failed to load mapping rules: {}", e.what());
        return false;
    }
}

bool MappingEngine::saveRulesToFile(const std::string& filename) {
    Logger::info("Saving mapping rules to: {}", filename);

    try {
        json j = json::array();
        for (const auto& rule : m_impl->rules) {
            json rj;
            rj["id"] = rule.id;
            rj["name"] = rule.name;
            const char* typeStr = "DIRECT";
            switch (rule.type) {
                case MappingType::CONDITIONAL: typeStr = "CONDITIONAL"; break;
                case MappingType::SEQUENCE: typeStr = "SEQUENCE"; break;
                case MappingType::TIMED: typeStr = "TIMED"; break;
                default: break;
            }
            rj["type"] = typeStr;
            const char* cmdStr = "UNKNOWN";
            switch (rule.atemCommand) {
                case AtemCommand::PROGRAM_CHANGE: cmdStr = "PROGRAM_CHANGE"; break;
                case AtemCommand::PREVIEW_CHANGE: cmdStr = "PREVIEW_CHANGE"; break;
                case AtemCommand::RECORD_START: cmdStr = "RECORD_START"; break;
                case AtemCommand::RECORD_STOP: cmdStr = "RECORD_STOP"; break;
                case AtemCommand::TALLY_CHANGE: cmdStr = "TALLY_CHANGE"; break;
                default: break;
            }
            rj["atemCommand"] = cmdStr;

            json conds = json::array();
            for (const auto& c : rule.conditions) {
                conds.push_back({{"type", c.type}, {"parameter", c.parameter},
                                 {"value", c.value}, {"comparison", c.comparison}});
            }
            rj["conditions"] = conds;

            json actions = json::array();
            for (const auto& a : rule.actions) {
                actions.push_back({{"command", a.command}, {"parameter", a.parameter},
                                   {"value", a.value}, {"delayMs", a.delayMs}});
            }
            rj["actions"] = actions;

            rj["enabled"] = rule.enabled;
            j.push_back(rj);
        }

        writeAtomicText(filename,j.dump(2));

        Logger::info("Saved {} mapping rules", m_impl->rules.size());
        return true;

    } catch (const std::exception& e) {
        Logger::error("Failed to save mapping rules: {}", e.what());
        return false;
    }
}

void MappingEngine::addRule(const MappingRule& rule) {
    m_impl->rules.push_back(rule);
    m_impl->ruleMap[rule.id] = rule;
    Logger::debug("Added mapping rule: {}", rule.name);
}

void MappingEngine::removeRule(const std::string& ruleId) {
    auto it = m_impl->ruleMap.find(ruleId);
    if (it != m_impl->ruleMap.end()) {
        auto& rules = m_impl->rules;
        rules.erase(std::remove_if(rules.begin(), rules.end(),
            [&](const MappingRule& r) { return r.id == ruleId; }), rules.end());

        m_impl->ruleMap.erase(it);
        Logger::debug("Removed mapping rule: {}", ruleId);
    }
}

void MappingEngine::updateRule(const MappingRule& rule) {
    removeRule(rule.id);
    addRule(rule);
}

std::vector<MappingRule> MappingEngine::getRules() const {
    return m_impl->rules;
}

MappingRule MappingEngine::getRule(const std::string& ruleId) const {
    auto it = m_impl->ruleMap.find(ruleId);
    if (it != m_impl->ruleMap.end()) {
        return it->second;
    }
    throw std::runtime_error("Rule not found: " + ruleId);
}

// Execute a single Sony action against the camera controller
static bool executeAction(const MappingAction& action, CameraController* camera) {
    if (!camera) {
        Logger::error("Cannot execute action '{}': no camera controller", action.command);
        return false;
    }

    if (action.command == "start_recording") return camera->startRecording();
    if (action.command == "stop_recording") return camera->stopRecording();
    if (action.command == "start_liveview") return camera->startLiveView();
    if (action.command == "stop_liveview") return camera->stopLiveView();

    if (action.command == "set_property") {
        int64_t value = 0;
        try {
            value = std::stoll(action.value, nullptr, 0);
        } catch (const std::exception&) {
            Logger::error("Invalid set_property value: '{}'", action.value);
            return false;
        }
        uint32_t code;
        try {
            code = static_cast<uint32_t>(std::stoul(action.parameter, nullptr, 0));
        } catch (const std::exception&) {
            Logger::error("Invalid set_property code: '{}'", action.parameter);
            return false;
        }
        return camera->setProperty(code, value);
    }

    Logger::error("Unknown mapping action command: '{}'", action.command);
    return false;
}

bool MappingEngine::processAtemCommand(AtemCommand command, const std::vector<std::string>& params, CameraController* target) {
    if(!target || command==AtemCommand::UNKNOWN) return false;
    Logger::debug("Processing ATEM command: {}", static_cast<int>(command));

    auto matchingRules = findMatchingRules(command, params);
    if (matchingRules.empty()) {
        Logger::debug("No matching rules found for ATEM command");
        return false;
    }

    bool anyExecuted = false;
    for (const auto& rule : matchingRules) {
        if (!rule.enabled || !validMappingRule(rule)) continue;

        Logger::info("Executing rule: {}", rule.name);

        for (const auto& action : rule.actions) {
            if (action.delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(action.delayMs));
            }
            if (executeAction(action, target)) {
                anyExecuted = true;
            }
        }
    }

    return anyExecuted;
}

bool MappingEngine::processAtemCommand(const std::string& rawCommand, CameraController* target) {
    auto command = AtemController::parseCommand(rawCommand);
    auto params = AtemController::parseParameters(rawCommand);
    return processAtemCommand(command, params, target);
}

std::vector<MappingRule> MappingEngine::findMatchingRules(AtemCommand command, const std::vector<std::string>& params) const {
    std::vector<MappingRule> matches;

    for (const auto& rule : m_impl->rules) {
        if (!rule.enabled) continue;
        if (rule.atemCommand != command) continue;

        // Evaluate conditions; all must pass. The only condition currently
        // supported is "atem_input" which compares the first command
        // parameter (the input number) with the expected value.
        bool allPass = true;
        for (const auto& cond : rule.conditions) {
            if (cond.type == "atem_input") {
                std::string actual = params.empty() ? "" : params[0];
                bool pass = false;
                if (cond.comparison == "equals") pass = actual == cond.value;
                else if (cond.comparison == "not_equals") pass = actual != cond.value;
                try {
                    int actualNum = std::stoi(actual);
                    int expectNum = std::stoi(cond.value);
                    if (cond.comparison == "greater") pass = actualNum > expectNum;
                    else if (cond.comparison == "less") pass = actualNum < expectNum;
                } catch (const std::exception&) {
                    // non-numeric compare already handled above
                }
                if (!pass) { allPass = false; break; }
            }
            else { allPass = false; break; }
            // Unknown conditions fail closed.
        }

        if (allPass) matches.push_back(rule);
    }

    return matches;
}
