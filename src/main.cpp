#include "common/Logger.h"
#include "service/BridgeService.h"
#include "ui/WebHost.h"

#include <windows.h>
#include <filesystem>

// 程序入口：初始化日志 → 启动桥接服务 → WebView2 宿主窗口（Element Plus 前端）→ 消息循环
int main(int argc, char* argv[])
{
    std::filesystem::create_directories(std::filesystem::u8path(BridgeService::resourcePath("logs")));
    Logger::init(LogLevel::INFO,BridgeService::resourcePath("logs/bridge.log"));

    try {
        BridgeService service;
        if (!service.startup()) {
            MessageBoxA(nullptr, "桥接服务启动失败，详情见日志文件。",
                        "ATEM Sony Bridge", MB_OK | MB_ICONERROR);
            return 1;
        }

        // WebView2 requires an STA on the thread that owns its windows.
        HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult)) {
            Logger::critical("COM initialization failed: 0x{:x}", (unsigned)comResult);
            return 1;
        }
        struct ComScope { BridgeService& service; ~ComScope() {service.shutdown();CoUninitialize();} } comScope{service};

        WebHost host;
        if (!host.create(service, GetModuleHandle(nullptr), SW_SHOW)) {
            Logger::error("宿主窗口创建失败");
            return 1;
        }

        int exitCode = host.messageLoop();
        // host is destroyed first, joining tasks before the service releases the SDK.
        return exitCode;

    } catch (const std::exception& e) {
        Logger::critical("未处理异常: {}", e.what());
        MessageBoxA(nullptr, e.what(), "ATEM Sony Bridge - 异常", MB_OK | MB_ICONERROR);
        return 1;
    }
}
