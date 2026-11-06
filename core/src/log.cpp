#include "kage/util/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace kage::log {
namespace {
std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
std::ofstream g_file;

const char* tag(Level l) {
    switch (l) {
        case Level::Debug: return "DBG";
        case Level::Info: return "INF";
        case Level::Warn: return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}
}  // namespace

void setLevel(Level l) { g_level = l; }
Level level() { return g_level; }

bool setFile(const std::string& path) {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    if (path.empty()) return true;
    g_file.open(path, std::ios::app);
    return g_file.is_open();
}

void write(Level l, std::string_view msg) {
#ifdef __ANDROID__
    static constexpr int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_print(prio[static_cast<int>(l)], "KageVR", "%.*s", static_cast<int>(msg.size()), msg.data());
#endif
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                                         static_cast<int>(ms), tag(l), msg);
    std::lock_guard lock(g_mutex);
#ifndef __ANDROID__
    std::fputs(line.c_str(), stderr);
#endif
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
}

}  // namespace kage::log
