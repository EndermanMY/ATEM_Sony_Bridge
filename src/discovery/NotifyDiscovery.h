#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

// NOTIFY 被动发现服务：
// 索尼相机在遥控拍摄等待状态会周期性向 239.255.255.250:1900 广播
// SSDP NOTIFY alive 包（SERVER: UPnP/1.0 SonyImagingDevice）。
// 在路由器组播受限的环境下（相机收不到 M-SEARCH 组播搜索），
// CrSDK 的枚举不可用，但相机→PC 方向的 NOTIFY 我们能稳定收到。
// 本服务被动监听 NOTIFY，从 LOCATION 字段提取相机 IP，供连接使用。
class NotifyDiscovery {
public:
    struct CameraEndpoint {
        std::string ip;
        std::string location;   // 完整 dd.xml 地址
        std::string uuid;
    };

    bool start();
    void stop();
    bool running() const { return m_running; }

    // 当前已发现的相机端点快照
    std::vector<CameraEndpoint> endpoints() const;

    // 新相机出现时的回调（任意线程触发）
    void setOnDiscovered(std::function<void(const CameraEndpoint&)> cb) { m_onDiscovered = std::move(cb); }

private:
    void listenLoop();

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::vector<CameraEndpoint> m_endpoints;
    std::function<void(const CameraEndpoint&)> m_onDiscovered;
};
