#pragma once
// =============================================================================
//  penhu/util/log.hpp
//  极简分级日志。
//
//  设计取舍：刻意不引 spdlog。理由是这个项目的日志需求只有三类
//  （启动配置、请求链路、致命错误），引一个 300KB+ 的库不值得；
//  而且服务端要写文件、CLI 要写控制台，自己包一层反而更可控。
//
//  线程安全：内部一把 mutex，写入是原子的。并发不高，够用。
// =============================================================================

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>
#include <memory>
#include <cstdio>
#include <source_location>

namespace penhu {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

const char* to_string(LogLevel level) noexcept;
bool parse_log_level(std::string_view text, LogLevel& out) noexcept;

class Logger {
public:
    /// level 以下不输出；log_file 为空则只输出到 stderr
    Logger(LogLevel level = LogLevel::Info, std::string log_file = {});
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept { level_ = level; }
    LogLevel level() const noexcept { return level_; }
    bool enabled(LogLevel level) const noexcept { return level >= level_; }

    /// 追加一个日志文件。
    ///
    /// 为什么需要一个 setter：GUI 程序没有控制台，出问题时唯一线索就是这个文件；
    /// 而日志文件路径要等 AppConfig 载入（依赖 exe 所在目录）才知道，
    /// 那时 default_logger 这个单例早就构造完了。
    /// 打开失败不抛异常——日志系统自己挂掉是最不该发生的事——只退回 stderr。
    void set_file(const std::string& log_file);

    static Logger& default_logger();

    void log(LogLevel level, const std::string& msg);
    void log(LogLevel level, const std::string& msg, const std::source_location& loc);

    void trace(const std::string& m) { log(LogLevel::Trace, m); }
    void debug(const std::string& m) { log(LogLevel::Debug, m); }
    void info (const std::string& m) { log(LogLevel::Info,  m); }
    void warn (const std::string& m) { log(LogLevel::Warn,  m); }
    void error(const std::string& m) { log(LogLevel::Error, m); }
    void fatal(const std::string& m) { log(LogLevel::Fatal, m); }

private:
    LogLevel                  level_;
    std::mutex                mutex_;
    std::unique_ptr<std::ofstream> file_;
};

/// 脱敏：日志里绝不出现明文密码 / 密钥 / token 全量
std::string mask_secret(std::string_view secret);

}  // namespace penhu
