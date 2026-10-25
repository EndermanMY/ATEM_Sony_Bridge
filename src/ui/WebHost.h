#pragma once

#include <windows.h>
#include <unknwn.h>
#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <atomic>
#include "nlohmann/json.hpp"
#include "common/TaskQueue.h"

class BridgeService;

#define WM_APP_WEBREPLY (WM_APP + 2)   // 工作线程结果回传 UI 线程

// WebView2 宿主窗口：
//  - 主窗口内嵌 WebView2（Element Plus 前端，加载本地 ui/index.html）
//  - 前端通过 window.chrome.webview.postMessage 发送 JSON-RPC：
//      { id, method, params }
//  - 宿主执行（耗时操作在工作线程），经 WM_APP_WEBREPLY 回 UI 线程后
//      PostWebMessageAsJson({ id, result }) 返回给前端
class WebHost {
public:
    WebHost();
    ~WebHost();   // Impl 为不完整类型，析构必须在 cpp 中定义
    bool create(BridgeService& service, HINSTANCE instance, int showCmd);
    int messageLoop();

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // 多机监看子窗口（独立 HWND + 独立 WebView2）
    static LRESULT CALLBACK monitorWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT monitorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void onMonitorEnvCreated(HRESULT hr, IUnknown* envRaw);
    void onMonitorControllerCreated(HRESULT hr, IUnknown* controllerRaw);
    void openMonitorWindow();
    // WebView2 异步回调入口（供 COM handler 调用）
    void onEnvCreated(HRESULT hr, IUnknown* env);
    void onControllerCreated(HRESULT hr, IUnknown* controller);

    void onWebMessage(const std::wstring& message, HWND source);
    void stopWorker();
    void flushFrames();
    // RPC id 保留原始 JSON 类型（前端发数字就回数字），否则前端回调匹配不上
    void dispatch(const nlohmann::json& id, const std::string& method, const nlohmann::json& params);
    void replyOnUiThread(const nlohmann::json& id, nlohmann::json result);   // 线程安全：投递到 UI 线程
    void replyDirect(const nlohmann::json& id, nlohmann::json result);       // 仅 UI 线程调用
    void runAsync(const nlohmann::json& id, std::function<nlohmann::json()> work);
    void svc_setDiscoveryPaused(bool paused);

    std::mutex m_routesMutex, m_framesMutex;
    TaskQueue m_controlTasks;
    std::atomic<bool> m_closing{false};
    bool m_framePending = false;
    long long m_nextRequest = 0;
    std::map<long long, std::pair<HWND,nlohmann::json>> m_routes;
    std::map<std::string,std::wstring> m_frames;
    std::shared_ptr<std::atomic<bool>> m_alive=std::make_shared<std::atomic<bool>>(true);
    unsigned m_monitorGeneration=0;
    HWND m_hwnd = nullptr;
    BridgeService* m_service = nullptr;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
