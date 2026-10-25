#include "NotifyDiscovery.h"
#include "../common/Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 从 SSDP 包的 LOCATION 头提取 http://ip:port/dd.xml 中的 IP
std::string extractLocationIp(const std::string& packet, std::string& locationOut) {
    size_t pos = packet.find("LOCATION:");
    if (pos == std::string::npos) pos = packet.find("Location:");
    if (pos == std::string::npos) return "";
    size_t lineEnd = packet.find("\r\n", pos);
    std::string loc = packet.substr(pos + 9, lineEnd - pos - 9);
    // 去空白
    while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) loc.erase(loc.begin());
    locationOut = loc;
    // http://10.0.0.177:64321/dd.xml -> 10.0.0.177
    size_t scheme = loc.find("://");
    if (scheme == std::string::npos) return "";
    size_t hostStart = scheme + 3;
    size_t hostEnd = loc.find(':', hostStart);
    if (hostEnd == std::string::npos) hostEnd = loc.find('/', hostStart);
    if (hostEnd == std::string::npos) hostEnd = loc.size();
    return loc.substr(hostStart, hostEnd - hostStart);
}

bool isSonyImagingDevice(const std::string& packet) {
    return packet.find("SonyImagingDevice") != std::string::npos;
}

} // namespace

bool NotifyDiscovery::start() {
    if (m_running) return true;
    m_running = true;
    m_thread = std::thread([this]() { listenLoop(); });
    Logger::info("NOTIFY 被动发现服务已启动（监听 SSDP 组播 239.255.255.250:1900）");
    return true;
}

void NotifyDiscovery::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

std::vector<NotifyDiscovery::CameraEndpoint> NotifyDiscovery::endpoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_endpoints;
}

void NotifyDiscovery::listenLoop() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        Logger::error("NotifyDiscovery: socket 创建失败");
        return;
    }

    // 复用端口（Windows SSDP 服务可能已占用 1900）并加入组播组
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1900);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Logger::warn("NotifyDiscovery: 绑定 1900 端口失败（{}），被动发现不可用", WSAGetLastError());
        closesocket(sock);
        m_running = false;
        return;
    }

    ip_mreq mreq{};
    inet_pton(AF_INET, "239.255.255.250", &mreq.imr_multiaddr.s_addr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    // 接收超时循环，便于周期性检查退出标志
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    char buf[4096];
    while (m_running) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string packet(buf);

        // 只关心索尼成像设备的 NOTIFY
        if (packet.find("NOTIFY") == std::string::npos) continue;
        if (!isSonyImagingDevice(packet)) continue;

        std::string location;
        std::string ip = extractLocationIp(packet, location);
        if (ip.empty()) continue;

        // 提取 UUID（USN 字段）
        std::string uuid;
        size_t usn = packet.find("USN:");
        if (usn != std::string::npos) {
            size_t lineEnd = packet.find("\r\n", usn);
            uuid = packet.substr(usn + 4, lineEnd - usn - 4);
            size_t s0 = uuid.find("uuid:");
            if (s0 != std::string::npos) uuid = uuid.substr(s0 + 5);
            while (!uuid.empty() && uuid.front() == ' ') uuid.erase(uuid.begin());
        }

        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool known = false;
            for (const auto& e : m_endpoints) {
                if (e.ip == ip) { known = true; break; }
            }
            if (!known) {
                m_endpoints.push_back({ip, location, uuid});
                isNew = true;
            }
        }
        if (isNew) {
            Logger::info("NOTIFY 发现索尼相机: {}", ip);
            auto cb = m_onDiscovered;
            if (cb) {
                CameraEndpoint ep{ip, location, uuid};
                cb(ep);
            }
        }
    }

    closesocket(sock);
}
