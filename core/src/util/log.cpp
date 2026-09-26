#include "penhu/util/log.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <array>
#include <algorithm>

namespace penhu {

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off:   return "OFF  ";
    }
    return "?????";
}

bool parse_log_level(std::string_view text, LogLevel& out) noexcept {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "trace") { out = LogLevel::Trace; return true; }
    if (lower == "debug") { out = LogLevel::Debug; return true; }
    if (lower == "info")  { out = LogLevel::Info;  return true; }
    if (lower == "warn" || lower == "warning") { out = LogLevel::Warn; return true; }
    if (lower == "error") { out = LogLevel::Error; return true; }
    if (lower == "fatal") { out = LogLevel::Fatal; return true; }
    if (lower == "off" || lower == "none") { out = LogLevel::Off; return true; }
    return false;
}

namespace {

/// 本地时间 "2026-09-18 16:38:38.123"
std::string timestamp_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif

    // 80 而不是 40：同 date.cpp 的理由 —— GCC 按 tm_year 的理论上界算，
    // 40 字节会被 -Wformat-truncation 警告。多几个字节的栈空间换干净输出。
    std::array<char, 80> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf.data());
}

}  // namespace

Logger::Logger(LogLevel level, std::string log_file) : level_(level) {
    // 复用 set_file，而不是在这里再写一份打开文件的逻辑：
    // 两份实现迟早会分叉（比如一边加了 flush、另一边没加），
    // 而日志行为不一致是最难发现的那类问题。
    set_file(log_file);
}

Logger::~Logger() = default;

void Logger::set_file(const std::string& log_file) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (log_file.empty()) {
        file_.reset();
        return;
    }
    auto opened = std::make_unique<std::ofstream>(log_file, std::ios::app);
    if (!opened->good()) {
        // 不抛：日志写不进去是「少了线索」，而不是「不该继续运行」。
        // 退回仅 stderr，并明确说一句，免得以后有人对着空日志文件猜。
        file_.reset();
        std::fprintf(stderr, "[WARN ] 无法打开日志文件 '%s'，回退到仅 stderr 输出\n",
                     log_file.c_str());
        return;
    }
    file_ = std::move(opened);
}

Logger& Logger::default_logger() {
    // 函数内静态：线程安全初始化，且避免了静态初始化顺序问题
    static Logger instance(LogLevel::Info);
    return instance;
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!enabled(level)) return;
    const std::string line =
        timestamp_now() + " [" + to_string(level) + "] " + msg + "\n";
    std::lock_guard<std::mutex> guard(mutex_);
    // 统一走 stderr：不需要为 stdout 刷缓冲，且不会污染 stdout 的机器可读输出
    std::fputs(line.c_str(), stderr);
    if (file_ && file_->good()) {
        (*file_) << line;
        // 每行 flush：崩溃时最后几条日志必须已经落盘，否则排查等于盲猜
        file_->flush();
    }
}

void Logger::log(LogLevel level, const std::string& msg, const std::source_location& loc) {
    if (!enabled(level)) return;
    std::string full = msg;
    full += "  @";
    full += loc.file_name();
    full += ':';
    full += std::to_string(loc.line());
    log(level, full);
}

std::string mask_secret(std::string_view secret) {
    if (secret.empty()) return "<empty>";
    if (secret.size() <= 8) return "****";
    std::string out(secret.substr(0, 4));
    out += "****";
    out += secret.substr(secret.size() - 2);
    out += "(" + std::to_string(secret.size()) + "B)";
    return out;
}

}  // namespace penhu
