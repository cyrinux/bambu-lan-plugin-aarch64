// Minimal thread-safe file logger shared by both plugin libraries.
#pragma once

#include <sstream>
#include <string>

namespace bbl_lan {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Directs log output to <dir>/<file_name>. Until called, logs go to stderr
// only when BBL_LAN_DEBUG is set in the environment.
void log_open(const std::string& dir, const std::string& file_name);
void log_write(LogLevel level, const std::string& message);

// Masks a secret for logging (keeps the first and last character).
std::string redact(const std::string& secret);

namespace detail {
template <typename... Args>
std::string concat(const Args&... args)
{
    std::ostringstream os;
    (os << ... << args);
    return os.str();
}
} // namespace detail

} // namespace bbl_lan

#define BBL_LOG(level, ...) ::bbl_lan::log_write(level, ::bbl_lan::detail::concat(__VA_ARGS__))
#define LOG_D(...) BBL_LOG(::bbl_lan::LogLevel::Debug, __VA_ARGS__)
#define LOG_I(...) BBL_LOG(::bbl_lan::LogLevel::Info, __VA_ARGS__)
#define LOG_W(...) BBL_LOG(::bbl_lan::LogLevel::Warn, __VA_ARGS__)
#define LOG_E(...) BBL_LOG(::bbl_lan::LogLevel::Error, __VA_ARGS__)
