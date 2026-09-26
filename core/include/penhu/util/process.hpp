#pragma once
// =============================================================================
//  penhu/util/process.hpp
//  子进程管理。主要服务于「本地模型部署」：起 llama-server、停它、解压运行时包。
//
//  两个刻意的实现决定：
//
//  1) 子进程的 stdout/stderr 一律重定向到文件，不用管道。
//     管道最经典的坑是「父进程没读、管道缓冲区满、子进程写阻塞卡死」。
//     llama-server 会持续往 stderr 打日志，用管道就是在赌它不会写满 64KB。
//     重定向到文件既没这个问题，日志还能留着排查。
//
//  2) 用 Job Object 管进程（Windows）。
//     llama-server 自己会 fork 出子线程/子进程做推理。只杀父进程会留下孤儿，
//     下次启动端口被占。Job Object 配 KILL_ON_JOB_CLOSE，句柄一关全清。
//
//  返回值用 Result，因为「启动失败」和「启动成功但立刻退出」是完全不同的
//  故障，调用方需要能区分（前者是路径错，后者是参数错或端口被占）。
// =============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "penhu/util/result.hpp"

namespace penhu::process {

/// 同步执行的结果
struct CommandResult {
    int         exit_code{-1};
    std::string output;         // stdout + stderr 合并
    int64_t     duration_ms{0};
    bool        timed_out{false};
};

/// 同步执行一个命令并等待结束。timeout_ms = 0 表示不超时（慎用）。
/// 输出通过临时文件收集，所以不会有管道填满导致的死锁。
Result<CommandResult> run(const std::string& exe,
                          const std::vector<std::string>& args,
                          const std::string& working_dir = {},
                          int timeout_ms = 0);

/// 命令是否存在（在 PATH 或给定绝对路径上）
bool command_exists(const std::string& exe);

/// 后台长驻进程句柄。析构时如果进程还在跑，会被强制结束。
class ChildProcess {
public:
    ChildProcess() noexcept = default;
    ~ChildProcess();

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// 启动。stdout/stderr 追加写入 stdout_log_path（留空则丢弃）。
    static Result<ChildProcess> spawn(const std::string& exe,
                                      const std::vector<std::string>& args,
                                      const std::string& working_dir,
                                      const std::string& stdout_log_path = {});

    /// 进程是否还在运行（会顺带回收结束状态）
    bool is_running() const;

    /// 温和终止：先请求退出，超过 grace_ms 未结束则强杀。
    Status terminate(int grace_ms = 5000);
    Status kill();

    /// 退出码；还在跑或没启动成功时为 -1
    int exit_code() const;

    int64_t pid() const noexcept { return pid_; }

    /// 等它结束。返回是否在 timeout 内结束。
    bool wait(int timeout_ms);

    bool valid() const noexcept { return pid_ > 0; }

private:
    void release() noexcept;

    int64_t pid_{0};
    void*   native_handle_{nullptr};   // Windows: HANDLE；POSIX: 留空
    mutable int exit_code_{-1};
    mutable bool exited_{false};
};

}  // namespace penhu::process
