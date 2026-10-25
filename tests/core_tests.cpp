#include "common/FrameUtils.h"
#include "common/Validation.h"
#include "common/TaskQueue.h"
#include "mapping/MappingPolicy.h"
#include <future>
#include <atomic>
#include "config/ConfigManager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try {
    require(extractJpeg({}).empty(),"empty frame");
    require(extractJpeg({0xff,0xd8,1}).empty(),"truncated frame");
    std::vector<uint8_t> input(176,0); input.insert(input.end(),{0xff,0xd8,1,2,0xff,0xd9,0,0});
    require(extractJpeg(input)==std::vector<uint8_t>({0xff,0xd8,1,2,0xff,0xd9}),"JPEG header/tail removal");
    auto temp=std::filesystem::current_path()/"bridge-core-test";
    std::filesystem::create_directories(temp);
    auto file=(temp/"config.json").u8string();
    ConfigManager config; auto c=config.getConfig();
    AppConfig::BoundCamera camera;camera.ip="192.0.2.1";camera.cameraId=1;camera.imageQuality=0;camera.compression=7;c.cameras.push_back(camera);config.setConfig(c);
    require(config.saveConfig(file),"save config");
    ConfigManager loaded;require(loaded.loadConfig(file),"reload config");
    require(loaded.getConfig().cameras.size()==1 && loaded.getConfig().cameras[0].compression==7,"camera prefs roundtrip");
    require(config.saveConfig(file),"repeat save");require(std::filesystem::exists(file+".bak"),"recoverable backup");
    require(!config.saveConfig((temp/"missing-parent"/"config.json").u8string()),"write failure detected");
    require(loaded.loadConfig(file),"previous config survives failed write");
    c.liveview.imageQuality=0;
    require(!config.commitConfig(c,(temp/"missing-parent"/"config.json").u8string()),"commit detects failure");
    require(config.getConfig().liveview.imageQuality==1,"failed commit rolls back memory");
    const uint8_t negative[]{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    for(int width:{1,2,4,8}) require(decodeInteger(negative,width,true)==-1,"signed width decoding");
    require(decodeInteger(negative,1,false)==255,"unsigned byte decoding");
    require(decodeInteger(negative,2,false)==65535,"unsigned word decoding");
    require(decodeInteger(negative,4,false)==4294967295LL,"unsigned dword decoding");
    require(fitsInteger(-128,1,true) && !fitsInteger(-129,1,true) && fitsInteger(255,1,false) && !fitsInteger(256,1,false) && !fitsInteger(-1,8,false),"integer width bounds");
    require(validIpv4("10.0.0.177") && !validIpv4("10.0.0.177x") && !validIpv4("10.0.0.1.2") && !validIpv4("256.0.0.1"),"strict IPv4 validation");
    require(allocateCameraId(c.cameras,"192.0.2.1",0)==1,"same camera retains ID");
    require(allocateCameraId(c.cameras,"192.0.2.2",0)==2,"next camera gets free ID");
    bool conflict=false;try{allocateCameraId(c.cameras,"192.0.2.2",1);}catch(const std::invalid_argument&){conflict=true;}
    require(conflict,"duplicate camera ID rejected");
    auto malformed=temp/"malformed.json";
    {std::ofstream out(malformed);out<<"{\"atem\":{\"port\":5},\"cameras\":[{\"ip\":\"bad\"}]}";}
    auto before=loaded.getConfig();require(!loaded.loadConfig(malformed.u8string()),"invalid config rejected");
    require(loaded.getConfig().atem.port==before.atem.port && loaded.getConfig().cameras[0].ip==before.cameras[0].ip,"invalid load preserves previous config");
    auto legacy=temp/"legacy.json";
    {std::ofstream out(legacy);out<<"{\"liveview\":{\"imageQuality\":0,\"compression\":9},\"cameras\":[{\"ip\":\"192.0.2.8\",\"modelId\":8}]}";}
    ConfigManager migration;require(migration.loadConfig(legacy.u8string()),"legacy migration");
    require(migration.getConfig().liveview.compression==9 && migration.getConfig().cameras[0].compression==-2 && migration.getConfig().cameras[0].cameraId==1,"legacy quality inherited and ID allocated");
    require(migration.saveConfig(legacy.u8string()) && migration.loadConfig(legacy.u8string()) && migration.getConfig().cameras[0].cameraId==1,"idempotent migration");
    TaskQueue queue;std::promise<void> entered,allowExit;auto gate=allowExit.get_future();std::atomic<bool> queuedRan{false};
    queue.post([&]{entered.set_value();gate.wait();});entered.get_future().wait();
    queue.post([&]{queuedRan=true;});
    auto stopped=std::async(std::launch::async,[&]{queue.stop();});
    require(stopped.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"shutdown waits for in-flight task");
    allowExit.set_value();stopped.get();
    require(!queuedRan && !queue.post([]{}),"shutdown discards queued tasks and rejects new work");
    MappingRule rule;rule.atemCommand=AtemCommand::PROGRAM_CHANGE;MappingAction action;action.command="start_recording";rule.actions.push_back(action);
    require(validMappingRule(rule),"valid mapping");action.command="unknown";rule.actions.push_back(action);
    require(!validMappingRule(rule),"entire mapping rejected before partial execution");
    rule.actions.pop_back();MappingCondition condition;condition.type="unknown";rule.conditions.push_back(condition);
    require(!validMappingRule(rule),"unknown mapping condition rejected");
    std::cout<<"Core regression cases passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
