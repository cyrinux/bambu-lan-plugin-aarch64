#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

namespace bbl_lan {
namespace {

std::mutex    g_mutex;
std::ofstream g_file;

const char* level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

std::string timestamp()
{
    const auto now   = std::chrono::system_clock::now();
    const auto t     = std::chrono::system_clock::to_time_t(now);
    const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm    tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(ms));
    return out;
}

} // namespace

void log_open(const std::string& dir, const std::string& file_name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open())
        return;
    g_file.open(dir + "/" + file_name, std::ios::app);
}

void log_write(LogLevel level, const std::string& message)
{
    static const bool to_stderr = std::getenv("BBL_LAN_DEBUG") != nullptr;
    if (level == LogLevel::Debug && !to_stderr)
        return;
    const std::string line = timestamp() + " [" + level_name(level) + "] " + message + "\n";
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
    if (to_stderr)
        std::fputs(line.c_str(), stderr);
}

std::string redact(const std::string& secret)
{
    if (secret.size() <= 2)
        return "**";
    return secret.substr(0, 1) + std::string(secret.size() - 2, '*') + secret.substr(secret.size() - 1);
}

} // namespace bbl_lan
