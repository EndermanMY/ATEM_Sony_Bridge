#include "WebHost.h"
#include "../service/BridgeService.h"
#include "../common/Logger.h"
#include "../sony/CameraController.h"

#include <winsock2.h>
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include "Version.h"

using namespace Microsoft::WRL;

// 前置声明：runAsync 使用（定义在文件后部）


// UTF-16 <-> UTF-8（前端消息含中文，必须正确转换）
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}
static std::wstring utf8ToWide(const std::string& u) {
    if (u.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &out[0], n);
    return out;
}

WebHost::WebHost() = default;
WebHost::~WebHost() { *m_alive=false;m_closing=true; stopWorker(); if(m_service) {m_service->stopLiveViewStream();m_service->setEventCallback({});} }

struct WebHost::Impl {
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    bool webReady = false;

    // 多机监看子窗口（独立 HWND + 独立 WebView2，加载 monitor.html）
    HWND monitorHwnd = nullptr;
    ComPtr<ICoreWebView2Environment> monEnv;
    ComPtr<ICoreWebView2Controller> monCtrl;
    ComPtr<ICoreWebView2> monWeb;
};

// WebView2 初始化流程（环境 → 控制器 → 消息订阅 → 加载前端）
void WebHost::onEnvCreated(HRESULT hr, IUnknown* envRaw) {
    if(m_closing || !m_impl) return;
    if(!IsWindow(m_hwnd)) return;
    if (FAILED(hr) || !envRaw) {
        Logger::critical("WebView2 环境创建失败: 0x{:x}（请确认系统已安装 WebView2 运行时）", (unsigned)hr);
        return;
    }
    m_impl->env = static_cast<ICoreWebView2Environment*>(envRaw);
    m_impl->env->CreateCoreWebView2Controller(m_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this,alive=m_alive](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if(!alive->load()) return S_OK;
                onControllerCreated(result, controller);
                return S_OK;
            }).Get());
}

void WebHost::onControllerCreated(HRESULT hr, IUnknown* controllerRaw) {
    if(m_closing || !m_impl) return;
    if(!IsWindow(m_hwnd)) return;
    if (FAILED(hr) || !controllerRaw) {
        Logger::critical("WebView2 控制器创建失败: 0x{:x}", (unsigned)hr);
        return;
    }
    m_impl->controller = static_cast<ICoreWebView2Controller*>(controllerRaw);
    m_impl->controller->get_CoreWebView2(&m_impl->web);

    // 前端消息订阅
    m_impl->web->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this,alive=m_alive](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if(!alive->load()) return S_OK;
                LPWSTR msg = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                    onWebMessage(msg, m_hwnd);
                    CoTaskMemFree(msg);
                }
                return S_OK;
            }).Get(), nullptr);

    // 加载本地前端（exe 旁 ui/index.html）
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring url = L"file:///" + dir + L"/ui/index.html";
    for (auto& ch : url) if (ch == L'\\') ch = L'/';

    // 导航诊断：开始/结束/文档状态都留日志，前端白屏时可直接定位
    m_impl->web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this,alive=m_alive](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if(!alive->load()) return S_OK;
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    Logger::info("导航开始: {}", wideToUtf8(uri));
                    CoTaskMemFree(uri);
                }
                return S_OK;
            }).Get(), nullptr);
    m_impl->web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this,alive=m_alive](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if(!alive->load()) return S_OK;
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (!success) {
                    COREWEBVIEW2_WEB_ERROR_STATUS err = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&err);
                    Logger::error("导航失败: web error status {}", (int)err);
                }
                return S_OK;
            }).Get(), nullptr);

    HRESULT hrNav = m_impl->web->Navigate(url.c_str());
    Logger::info("Navigate({}) -> 0x{:x}", wideToUtf8(url), (unsigned)hrNav);
    m_impl->webReady = true;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_impl->controller->put_Bounds(rc);
    Logger::info("WebView2 就绪，前端已加载");
}

LRESULT CALLBACK WebHost::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (WebHost*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProc(hwnd, msg, wp, lp);
    return self->wndProc(hwnd, msg, wp, lp);
}

bool WebHost::create(BridgeService& service, HINSTANCE instance, int showCmd) {
    m_service = &service;
    m_impl = std::make_unique<Impl>();

    // 事件推送通道：后端工作线程的监看帧等事件 → 投递回 UI 线程 → 广播到所有窗口
    // （WM_APP_WEBREPLY 的 payload 同为 JSON 字符串，前端按"带 id 走 RPC / 带 event 走事件"区分）
    service.setEventCallback([this](const std::string& json) {
        if(m_closing) return;
        auto data=nlohmann::json::parse(json);
        std::lock_guard<std::mutex> lock(m_framesMutex);
        m_frames[data.value("ip",std::string())]=utf8ToWide(json);
        if(!m_framePending) { m_framePending=true; if(!PostMessage(m_hwnd,WM_APP+3,0,0)) {m_framePending=false;m_frames.clear();} }
    });

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(11, 14, 20));
    wc.lpszClassName = L"AtemSonyBridgeHostWnd";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowW(wc.lpszClassName, L"ATEM Sony Bridge " BRIDGE_VERSION_W,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1240, 800,
        nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;
    ShowWindow(m_hwnd, showCmd);
    UpdateWindow(m_hwnd);

    // WebView2 用户数据目录（exe 旁）
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring userData = exePath;
    userData = userData.substr(0, userData.find_last_of(L"\\/")) + L"\\WebView2Data";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this,alive=m_alive](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if(!alive->load()) return S_OK;
                onEnvCreated(result, env);
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        Logger::critical("CreateCoreWebView2EnvironmentWithOptions 失败: 0x{:x}", (unsigned)hr);
        return false;
    }
    return true;
}

// ---------------- 多机监看子窗口 ----------------
LRESULT CALLBACK WebHost::monitorWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (WebHost*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProc(hwnd, msg, wp, lp);
    return self->monitorWndProc(hwnd, msg, wp, lp);
}

LRESULT WebHost::monitorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            if (m_impl && m_impl->monCtrl) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                m_impl->monCtrl->put_Bounds(rc);
            }
            return 0;
        }
        case WM_APP_WEBREPLY: {
            if (m_impl && m_impl->monWeb) {
                auto* payload = (std::wstring*)wp;
                m_impl->monWeb->PostWebMessageAsJson(payload->c_str());
            }
            delete reinterpret_cast<std::wstring*>(wp);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            {std::lock_guard<std::mutex> lock(m_routesMutex);
             for(auto it=m_routes.begin();it!=m_routes.end();) {if(it->second.first==hwnd) it=m_routes.erase(it);else ++it;}}
            {MSG queued;while(PeekMessage(&queued,hwnd,WM_APP_WEBREPLY,WM_APP_WEBREPLY,PM_REMOVE))delete reinterpret_cast<std::wstring*>(queued.wParam);}
            ++m_monitorGeneration;
            // 关闭监看窗口：停掉推流并清理（不影响主窗口）
            if (m_impl) {
                m_impl->monWeb = nullptr;
                m_impl->monCtrl = nullptr;
                m_impl->monEnv = nullptr;
                m_impl->monitorHwnd = nullptr;
            }
            if(!m_closing) {
                m_controlTasks.post([this]{m_service->stopLiveViewStream();});
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void WebHost::onMonitorEnvCreated(HRESULT hr, IUnknown* envRaw) {
    if(m_closing || !m_impl) return;
    if(!m_impl->monitorHwnd || !IsWindow(m_impl->monitorHwnd)) return;
    if (FAILED(hr) || !envRaw) {
        Logger::error("监看窗口 WebView2 环境创建失败: 0x{:08x}", (unsigned)hr);
        return;
    }
    m_impl->monEnv = static_cast<ICoreWebView2Environment*>(envRaw);
    m_impl->monEnv->CreateCoreWebView2Controller(m_impl->monitorHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this,alive=m_alive,generation=m_monitorGeneration](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if(!alive->load() || generation!=m_monitorGeneration) return S_OK;
                onMonitorControllerCreated(result, controller);
                return S_OK;
            }).Get());
}

void WebHost::onMonitorControllerCreated(HRESULT hr, IUnknown* controllerRaw) {
    if(m_closing || !m_impl) return;
    if(!m_impl->monitorHwnd || !IsWindow(m_impl->monitorHwnd)) return;
    if (FAILED(hr) || !controllerRaw) {
        Logger::error("监看窗口 WebView2 控制器创建失败: 0x{:08x}", (unsigned)hr);
        m_impl->monitorHwnd = nullptr;
        return;
    }
    m_impl->monCtrl = static_cast<ICoreWebView2Controller*>(controllerRaw);
    m_impl->monCtrl->get_CoreWebView2(&m_impl->monWeb);

    // RPC 与事件与主窗口同一套分发（回包广播到所有窗口）
    m_impl->monWeb->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this,alive=m_alive](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if(!alive->load()) return S_OK;
                LPWSTR msg = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                    onWebMessage(msg, m_impl->monitorHwnd);
                    CoTaskMemFree(msg);
                }
                return S_OK;
            }).Get(), nullptr);

    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring url = L"file:///" + dir + L"/ui/monitor.html";
    for (auto& ch : url) if (ch == L'\\') ch = L'/';
    m_impl->monWeb->Navigate(url.c_str());

    RECT rc;
    GetClientRect(m_impl->monitorHwnd, &rc);
    m_impl->monCtrl->put_Bounds(rc);
    ShowWindow(m_impl->monitorHwnd, SW_SHOW);
    Logger::info("多机监看窗口已打开");
}

void WebHost::openMonitorWindow() {
    if (m_impl->monitorHwnd) {
        // 已打开：前置即可
        SetForegroundWindow(m_impl->monitorHwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = monitorWndProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(11, 14, 20));
        wc.lpszClassName = L"AtemSonyBridgeMonitorWnd";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND hwnd = CreateWindowW(L"AtemSonyBridgeMonitorWnd", L"多机监看 — ATEM Sony Bridge",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 720,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) {
        Logger::error("监看窗口创建失败");
        return;
    }
    m_impl->monitorHwnd = hwnd;
    ++m_monitorGeneration;
    ShowWindow(hwnd, SW_SHOW);

    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring userData = exePath;
    userData = userData.substr(0, userData.find_last_of(L"\\/")) + L"\\WebView2Data";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this,alive=m_alive,generation=m_monitorGeneration](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if(!alive->load() || generation!=m_monitorGeneration) return S_OK;
                onMonitorEnvCreated(result, env);
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        Logger::error("监看窗口环境创建失败: 0x{:08x}", (unsigned)hr);
        m_impl->monitorHwnd = nullptr;
    }
}

int WebHost::messageLoop() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

LRESULT WebHost::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP+3: flushFrames(); return 0;
        case WM_CLOSE:
            m_closing=true; stopWorker(); m_service->stopLiveViewStream();
            if(m_impl && m_impl->monitorHwnd) DestroyWindow(m_impl->monitorHwnd);
            {std::lock_guard<std::mutex> lock(m_routesMutex);m_routes.clear();}
            {MSG queued;while(PeekMessage(&queued,hwnd,WM_APP_WEBREPLY,WM_APP_WEBREPLY,PM_REMOVE))delete reinterpret_cast<std::wstring*>(queued.wParam);}
            if(m_impl->controller) m_impl->controller->Close();
            DestroyWindow(hwnd); return 0;
        case WM_SIZE: {
            if (m_impl && m_impl->controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                m_impl->controller->put_Bounds(rc);
            }
            return 0;
        }
        case WM_APP_WEBREPLY: {
            // 工作线程回传的结果（堆上 JSON 字符串），转发给前端
            auto* payload = (std::wstring*)wp;
            if (m_impl && m_impl->web) {
                m_impl->web->PostWebMessageAsJson(payload->c_str());
            }
            delete payload;
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ============================ JSON-RPC 分发 ============================
void WebHost::onWebMessage(const std::wstring& message, HWND source) {
    try {
        auto j = nlohmann::json::parse(wideToUtf8(message));
        // id 保留原始 JSON 类型：前端发数字，回包必须也是数字，否则回调匹配不上
        nlohmann::json id = j.contains("id") ? j["id"] : nlohmann::json(0);
        std::string method = j.value("method", std::string());
        nlohmann::json params = j.value("params", nlohmann::json::object());
        if(m_closing) return;
        long long route;
        {std::lock_guard<std::mutex> lock(m_routesMutex); route=++m_nextRequest; m_routes.emplace(route,std::make_pair(source,id));}
        try { dispatch(nlohmann::json(route), method, params); }
        catch(const std::exception& e) { replyOnUiThread(nlohmann::json(route),{{"ok",false},{"code","INVALID_REQUEST"},{"message",e.what()}}); }
    } catch (const std::exception& e) {
        Logger::error("前端消息解析失败: {}", e.what());
    }
}

// 线程安全回包：把 JSON 字符串投递回 UI 线程
// Internal request IDs preserve the origin HWND and original client ID.
void WebHost::replyOnUiThread(const nlohmann::json& id, nlohmann::json result) {
    HWND target=nullptr; nlohmann::json original;
    std::lock_guard<std::mutex> lock(m_routesMutex);
     auto it=m_routes.find(id.get<long long>()); if(it==m_routes.end()) return;
     target=it->second.first;original=it->second.second;m_routes.erase(it);
    if(result.is_array()) result=nlohmann::json{{"cameras",result}};
    if(!result.contains("ok")) result["ok"]=true;
    if(!result.contains("code")) result["code"]=result["ok"].get<bool>()?"OK":"OPERATION_FAILED";
    if(!result.contains("message")) result["message"]=result["ok"].get<bool>()?"Completed":"Operation failed";
    auto* payload=new std::wstring(utf8ToWide(nlohmann::json{{"id",original},{"result",result}}.dump()));
    if(m_closing || !IsWindow(target) || !PostMessage(target,WM_APP_WEBREPLY,(WPARAM)payload,0)) delete payload;
}

void WebHost::replyDirect(const nlohmann::json& id, nlohmann::json result) {
    replyOnUiThread(id, std::move(result));   // 统一走消息队列，避免跨线程 COM
}

void WebHost::runAsync(const nlohmann::json& id, std::function<nlohmann::json()> work) {
    if(!m_controlTasks.post([this,id,work] {
        svc_setDiscoveryPaused(true);
        try { replyOnUiThread(id,work()); }
        catch(const std::exception& e) { replyOnUiThread(id,{{"ok",false},{"code","OPERATION_FAILED"},{"message",e.what()}}); }
        svc_setDiscoveryPaused(false);
    })) replyOnUiThread(id,{{"ok",false},{"code","SHUTTING_DOWN"},{"message","Application is closing"}});
}
void WebHost::stopWorker() { m_controlTasks.stop(); }
void WebHost::flushFrames() {
    std::map<std::string,std::wstring> frames;
    {std::lock_guard<std::mutex> lock(m_framesMutex);frames.swap(m_frames);m_framePending=false;}
    if(m_impl && m_impl->monWeb) for(const auto& entry:frames) m_impl->monWeb->PostWebMessageAsJson(entry.second.c_str());
}

void WebHost::svc_setDiscoveryPaused(bool paused) {
    if (m_service) m_service->setDiscoveryPaused(paused);
}

void WebHost::dispatch(const nlohmann::json& id, const std::string& method, const nlohmann::json& p) {
    BridgeService& svc = *m_service;

    // ---- 即时查询（快，直接执行）----
    if (method == "status") { replyDirect(id, svc.getStatus()); return; }
    if (method == "models.list") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : CameraController::modelNames()) arr.push_back(n);
        replyDirect(id, { {"models", arr} });
        return;
    }
    if (method == "logs") {
        nlohmann::json entries = nlohmann::json::array();
        for (const auto& e : Logger::recentEntries(300)) {
            entries.push_back({ {"time", e.timeStr},
                                {"level", std::string(e.level == LogLevel::WARN ? "WARN" :
                                            e.level == LogLevel::ERR ? "ERROR" :
                                            e.level == LogLevel::CRIT ? "CRIT" : e.level==LogLevel::TRACE ? "TRACE" : e.level==LogLevel::DEBUG ? "DEBUG" : "INFO")},
                                {"text", e.text} });
        }
        replyDirect(id, { {"ok", true}, {"entries", entries} });
        return;
    }

    // ---- 耗时操作（工作线程）----
    if (method == "cameras.discover") {
        int timeoutSec = p.value("timeoutSec", 6);
        int modelId = p.value("modelId", 8);
        svc.setDefaultModelId(modelId);
        runAsync(id, [&svc, timeoutSec]() {
            auto cams = svc.discoverCamerasSync(timeoutSec);
            // 合并 NOTIFY 被动发现（去重）
            for (const auto& n : svc.notifyEndpoints()) {
                bool dup = false;
                for (const auto& c : cams) if (c.ipAddress == n.ipAddress) dup = true;
                if (!dup) cams.push_back(n);
            }
            nlohmann::json arr = nlohmann::json::array();
            int idx = 0;
            for (const auto& c : cams) {
                arr.push_back({ {"index", idx++},
                                {"model", c.model}, {"serialNumber", c.serialNumber},
                                {"connectionType", c.connectionType}, {"ipAddress", c.ipAddress} });
            }
            return nlohmann::json{ {"ok", true}, {"cameras", arr} };
        });
        return;
    }
    if (method == "cameras.add") {
        std::string ip = p.value("ip", std::string());
        int modelId = p.value("modelId", 8);
        std::string name = p.value("name", std::string("Sony 相机"));
        std::string su = p.value("sshUser", std::string());
        std::string sp = p.value("sshPass", std::string());
        int cameraId = p.value("cameraId", 0);
        runAsync(id, [&svc, ip, modelId, name, su, sp, cameraId]() {
            svc.addBoundCamera(ip,modelId,name,su,sp,cameraId);
            return svc.connectCameraByIp(ip, modelId, su, sp);
        });
        return;
    }
    if (method == "cameras.connectIndex") {
        int index = p.value("index", 0);
        std::string su = p.value("sshUser", std::string());
        std::string sp = p.value("sshPass", std::string());
        runAsync(id, [&svc, index, su, sp]() { return svc.connectCamera(index, su, sp); });
        return;
    }
    if (method == "cameras.remove") { auto ip=p.value("ip",std::string());runAsync(id,[&svc,ip]{return svc.removeBoundCamera(ip);}); return; }
    if (method == "cameras.rename") {
        auto ip=p.value("ip",std::string()),name=p.value("name",std::string());
        runAsync(id,[&svc,ip,name]{return nlohmann::json{{"ok",svc.renameBoundCamera(ip,name)}};});
        return;
    }
    if (method == "cameras.setId") {
        auto ip=p.value("ip",std::string());int cameraId=p.value("cameraId",0);
        runAsync(id,[&svc,ip,cameraId]{return nlohmann::json{{"ok",svc.setBoundCameraId(ip,cameraId)},{"code","CAMERA_ID_VALIDATION"},{"message","Camera ID must be unique and within 1-15"}};});
        return;
    }
    if (method == "cameras.connectByIp") {
        std::string ip = p.value("ip", std::string());
        int modelId = p.value("modelId", 8);
        std::string su = p.value("sshUser", std::string());
        std::string sp = p.value("sshPass", std::string());
        runAsync(id, [&svc, ip, modelId, su, sp]() { return svc.connectCameraByIp(ip, modelId, su, sp); });
        return;
    }
    if(method=="cameras.disconnect") {auto ip=p.value("ip",std::string());runAsync(id,[&svc,ip]{return svc.disconnectCamera(ip);});return;}
    if (method == "atem.connect") {
        std::string ip = p.value("ip", std::string());
        int port = p.value("port", 9910);
        runAsync(id, [&svc, ip, port]() { return svc.connectAtem(ip, port); });
        return;
    }
    if (method == "atem.disconnect") { runAsync(id, [&svc]() { return svc.disconnectAtem(); }); return; }
    if (method == "record.start") {
        std::string ip = p.value("ip", std::string());
        runAsync(id, [&svc, ip]() { return svc.startRecording(ip); });
        return;
    }
    if (method == "record.stop") {
        std::string ip = p.value("ip", std::string());
        runAsync(id, [&svc, ip]() { return svc.stopRecording(ip); });
        return;
    }
    if (method == "liveview.start") {
        runAsync(id,[&svc]{svc.startLiveViewStreams();return nlohmann::json{{"ok",svc.isLiveViewStreaming()}};});
        return;
    }
    if (method == "liveview.stop") {
        runAsync(id,[&svc]{svc.stopLiveViewStream();return nlohmann::json{{"ok",true}};});
        return;
    }
    if (method == "liveview.setQuality") {
        std::string ip = p.value("ip", std::string());
        int quality=p.value("imageQuality",1), compression=p.value("compression",-1);
        runAsync(id,[&svc,ip,quality,compression]{return svc.setLiveviewQuality(ip,quality,compression);});
        return;
    }
    if (method == "liveview.quality") {
        std::string ip = p.value("ip", std::string());
        runAsync(id,[&svc,ip]{return svc.getLiveviewQuality(ip);});
        return;
    }
    if (method == "cameras.setActive") {
        svc.setActiveCamera(p.value("ip", std::string()));
        replyDirect(id, { {"ok", true} });
        return;
    }
    if (method == "monitor.open") {
        openMonitorWindow();
        replyDirect(id, { {"ok", true} });
        return;
    }
    if (method == "props.list") {
        std::string ip = p.value("ip", std::string());
        runAsync(id, [&svc, ip]() { return svc.getProperties(ip); });
        return;
    }
    if (method == "props.set") {
        std::string ip = p.value("ip", std::string());
        uint32_t code = p.value("code", 0u);
        if(!p.contains("value") || !p["value"].is_number()) throw std::invalid_argument("Property value must be an integer");
        double number=p["value"].get<double>();
        if(!std::isfinite(number) || std::floor(number)!=number || std::abs(number)>9007199254740991.0) throw std::invalid_argument("Property value exceeds exact integer range");
        int64_t value=static_cast<int64_t>(number);
        runAsync(id, [&svc, ip, code, value]() { return svc.setProperty(ip, code, value); });
        return;
    }

    replyDirect(id, { {"ok", false}, {"message", "unknown method: " + method} });
}
