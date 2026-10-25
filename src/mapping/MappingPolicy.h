#pragma once
#include "MappingEngine.h"
#include <limits>

inline bool validMappingRule(const MappingRule& rule) {
    if(rule.atemCommand==AtemCommand::UNKNOWN || rule.actions.empty() || rule.type==MappingType::TIMED) return false;
    for(const auto& condition:rule.conditions) {
        if(condition.type!="atem_input") return false;
        if(condition.comparison!="equals" && condition.comparison!="not_equals" && condition.comparison!="greater" && condition.comparison!="less") return false;
    }
    for(const auto& action:rule.actions) {
        if(action.delayMs<0 || action.delayMs>5000) return false;
        if(action.command=="set_property") {
            try {
                size_t codeEnd=0,valueEnd=0;
                auto code=std::stoll(action.parameter,&codeEnd,0);
                (void)std::stoll(action.value,&valueEnd,0);
                if(code<=0 || code>std::numeric_limits<uint32_t>::max() || codeEnd!=action.parameter.size() || valueEnd!=action.value.size()) return false;
            } catch(const std::exception&) {return false;}
        } else if(action.command!="start_recording" && action.command!="stop_recording" && action.command!="start_liveview" && action.command!="stop_liveview") return false;
    }
    return true;
}
