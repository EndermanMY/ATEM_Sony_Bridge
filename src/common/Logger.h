#pragma once

#include <string>
#include <memory>
#include <functional>
#include <iostream>
#include <sstream>
#include <mutex>
#include <vector>
#include <deque>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

// Simple logger implementation (placeholder for spdlog)
// Note: enumerators avoid "ERROR"/"CRITICAL" which clash with wingdi macros.
enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERR,
    CRIT
};

class Logger {
public:
    struct Entry {
        LogLevel level;
        std::string text;      // formatted, without level prefix
        std::string timeStr;   // "HH:MM:SS"
    };

    static void init(LogLevel level = LogLevel::INFO,
                     const std::string& filename = "atem_sony_bridge.log") {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_level = level;
        s_logFile = filename;
        if(s_fileHandle) fclose(s_fileHandle);
#ifdef _WIN32
        _wfopen_s(&s_fileHandle,std::filesystem::u8path(filename).c_str(),L"a");
#else
        s_fileHandle = fopen(filename.c_str(), "a");
#endif
    }

    static LogLevel level() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_level;
    }

    static void setLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_level = level;
    }

    static void setEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_enabled = enabled;
    }

    // Web UI / other listeners get every formatted entry in real time.
    using Listener = std::function<void(const Entry&)>;
    static void addListener(Listener listener) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_listeners.push_back(std::move(listener));
    }

    // Ring buffer with the most recent entries (for late UI subscribers).
    static std::vector<Entry> recentEntries(size_t maxCount = 200) {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<Entry> out;
        size_t start = s_ring.size() > maxCount ? s_ring.size() - maxCount : 0;
        for (size_t i = start; i < s_ring.size(); ++i) out.push_back(s_ring[i]);
        return out;
    }

    template<typename... Args>
    static void trace(const char* fmt, Args&&... args) {
        log(LogLevel::TRACE, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) {
        log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void info(const char* fmt, Args&&... args) {
        log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void warn(const char* fmt, Args&&... args) {
        log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void error(const char* fmt, Args&&... args) {
        log(LogLevel::ERR, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void critical(const char* fmt, Args&&... args) {
        log(LogLevel::CRIT, fmt, std::forward<Args>(args)...);
    }

    // Overloads for plain strings without formatting
    static void trace(const char* msg) { write(LogLevel::TRACE, msg); }
    static void debug(const char* msg) { write(LogLevel::DEBUG, msg); }
    static void info(const char* msg) { write(LogLevel::INFO, msg); }
    static void warn(const char* msg) { write(LogLevel::WARN, msg); }
    static void error(const char* msg) { write(LogLevel::ERR, msg); }
    static void critical(const char* msg) { write(LogLevel::CRIT, msg); }

private:
    // ---- {} 风格格式化（兼容 spdlog 占位写法，支持 {:x} 十六进制）----
    static void appendArg(std::ostringstream& oss, char spec, const std::string& v) {
        (void)spec; oss << v;
    }
    template<typename T>
    static void appendArg(std::ostringstream& oss, char spec, const T& v) {
        if (spec == 'x' || spec == 'X') oss << std::hex << (unsigned long long)v << std::dec;
        else if (spec == 'd') oss << (long long)v;
        else oss << v;
    }

    // 递归终止：没有剩余参数，剩余占位符原样输出
    static void formatRecurse(std::ostringstream& oss, const char* p) {
        oss << p;
    }

    template<typename T, typename... Rest>
    static void formatRecurse(std::ostringstream& oss, const char* p, T&& v, Rest&&... rest) {
        // 找下一个 {} 或 {:x} 占位符
        const char* brace = strchr(p, '{');
        while (brace && !(brace[1] == '}' || brace[1] == ':')) brace = strchr(brace + 1, '{');
        if (!brace) { oss << p; return; }
        oss.write(p, brace - p);
        char spec = brace[1] == ':' ? brace[2] : 0;
        appendArg(oss, spec, v);
        p = brace[1] == ':' ? brace + 4 : brace + 2;
        formatRecurse(oss, p, std::forward<Rest>(rest)...);
    }

    template<typename... Args>
    static void log(LogLevel level, const char* fmt, Args&&... args) {
        if (static_cast<int>(level) < static_cast<int>(s_level)) return;
        std::ostringstream oss;
        formatRecurse(oss, fmt, std::forward<Args>(args)...);
        write(level, oss.str().c_str());
    }

    static void write(LogLevel level, const char* text) {
        const char* levelStr = "";
        switch (level) {
            case LogLevel::TRACE: levelStr = "TRACE"; break;
            case LogLevel::DEBUG: levelStr = "DEBUG"; break;
            case LogLevel::INFO: levelStr = "INFO"; break;
            case LogLevel::WARN: levelStr = "WARN "; break;
            case LogLevel::ERR: levelStr = "ERROR"; break;
            case LogLevel::CRIT: levelStr = "CRIT "; break;
        }

        Entry entry;
        entry.level = level;
        entry.text = text;
        entry.timeStr = currentTime();

        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_enabled) return;
        s_ring.push_back(entry);
        while (s_ring.size() > s_ringMax) s_ring.pop_front();

        std::cout << "[" << entry.timeStr << "] [" << levelStr << "] " << text << std::endl;
        if (s_fileHandle) {
            fprintf(s_fileHandle, "[%s] [%s] %s\n", entry.timeStr.c_str(), levelStr, text);
            fflush(s_fileHandle);
        }
        for (const auto& listener : s_listeners) listener(entry);
    }

    static std::string currentTime() {
        using clock = std::chrono::system_clock;
        auto now = clock::to_time_t(clock::now());
        struct tm localTm {};
#ifdef _WIN32
        localtime_s(&localTm, &now);
#else
        localtime_r(&now, &localTm);
#endif
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
        return buf;
    }

    static LogLevel s_level;
    static bool s_enabled;
    static std::string s_logFile;
    static FILE* s_fileHandle;
    static std::mutex s_mutex;
    static std::deque<Entry> s_ring;
    static const size_t s_ringMax = 1000;
    static std::vector<Listener> s_listeners;
};

inline LogLevel Logger::s_level = LogLevel::INFO;
inline bool Logger::s_enabled = true;
inline std::string Logger::s_logFile;
inline FILE* Logger::s_fileHandle = nullptr;
inline std::mutex Logger::s_mutex;
inline std::deque<Logger::Entry> Logger::s_ring;
inline const size_t Logger::s_ringMax;
inline std::vector<Logger::Listener> Logger::s_listeners;
