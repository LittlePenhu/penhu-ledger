#include "penhu/util/process.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/util/fs.hpp"
#include "penhu/util/log.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstdlib>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace penhu::process {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#if defined(_WIN32)

/// 句柄捆绑：Job Object + 进程句柄。
/// 两者都需要：Job 负责「关掉就清干净整棵进程树」，
/// 进程句柄负责「读退出码 / 等它结束」。少任何一个都得另外想办法。
struct NativeImpl {
    HANDLE job{nullptr};
    HANDLE process{nullptr};
};

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                          out.data(), len);
    return out;
}

/// 按 Windows 命令行规则转义参数。
/// 细节在于反斜杠：它只在双引号前才需要加倍，而参数末尾的反斜杠必须加倍，
/// 否则 "C:\some dir\" 里的引号会被吃掉，后面所有参数跟着错位。
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::string& exe,
                                const std::vector<std::string>& args) {
    std::wstring cmd = quote_arg(to_wide(exe));
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd += quote_arg(to_wide(a));
    }
    return cmd;
}

std::string last_error_text() {
    const DWORD code = ::GetLastError();
    if (code == 0) return "无错误信息";

    LPWSTR buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::string message;
    if (len > 0 && buffer != nullptr) {
        const int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, buffer,
                                                  static_cast<int>(len), nullptr, 0,
                                                  nullptr, nullptr);
        if (utf8_len > 0) {
            message.resize(static_cast<size_t>(utf8_len));
            ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len),
                                  message.data(), utf8_len, nullptr, nullptr);
        }
    }
    if (buffer != nullptr) ::LocalFree(buffer);

    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message.empty() ? ("Windows 错误码 " + std::to_string(code)) : message;
}

/// 把 Job Object 挂到进程上，配成「句柄关闭即杀光整棵树」
HANDLE attach_job(HANDLE process_handle) {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));

    if (!::AssignProcessToJobObject(job, process_handle)) {
        // 不致命（进程还是能跑），但强杀可能漏掉子进程，必须留痕
        Logger::default_logger().warn(
            "无法把进程加入 Job Object（强杀时可能残留子进程）: " + last_error_text());
        ::CloseHandle(job);
        return nullptr;
    }
    return job;
}

HANDLE open_log_handle(const std::string& path, bool append) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return ::CreateFileW(to_wide(path).c_str(),
                         FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &sa,
                         append ? OPEN_ALWAYS : CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

/// 统一启动路径，run() 与 spawn() 共用
struct SpawnResult {
    bool ok{false};
    std::string error;
    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    DWORD  pid{0};
};

SpawnResult spawn_raw(const std::wstring& cmdline,
                      const std::wstring& workdir,
                      HANDLE out_handle,
                      DWORD extra_flags) {
    SpawnResult out;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    if (out_handle != INVALID_HANDLE_VALUE) {
        si.hStdOutput = out_handle;
        si.hStdError = out_handle;
    } else {
        si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    }
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                     TRUE, CREATE_NO_WINDOW | extra_flags, nullptr,
                                     workdir.empty() ? nullptr : workdir.c_str(),
                                     &si, &pi);
    if (!ok) {
        out.error = last_error_text();
        return out;
    }
    out.ok = true;
    out.process = pi.hProcess;
    out.thread = pi.hThread;
    out.pid = pi.dwProcessId;
    return out;
}

#else

/// POSIX 下只需要记住子进程的 pid。
/// （Windows 那边要 Job + HANDLE 两样：Job 负责「关掉就清干净整棵进程树」。
///   POSIX 有进程组，kill(-pid) 一行就等价于那个 Job。）
///
/// pid 字段在进程被回收后会置 -1：**waitpid 同一个子进程只能成功一次**，
/// 重复调用会拿到 ECHILD，所以回收过就必须标记，否则退出码会被后面的调用覆盖成 -1。
struct NativeImpl {
    pid_t pid{-1};
};
#endif

}  // namespace

bool command_exists(const std::string& exe) {
    if (exe.empty()) return false;
    if (exe.find('/') != std::string::npos || exe.find('\\') != std::string::npos) {
        return fs::is_regular_file(exe);
    }
#if defined(_WIN32)
    const wchar_t* path_env = _wgetenv(L"PATH");
    if (path_env == nullptr) return false;

    std::wstringstream ss{std::wstring(path_env)};
    std::wstring dir;
    const std::wstring target = to_wide(exe);
    while (std::getline(ss, dir, L';')) {
        if (dir.empty()) continue;
        std::wstring candidate = dir;
        if (candidate.back() != L'\\' && candidate.back() != L'/') candidate.push_back(L'\\');
        candidate += target;
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
    }
    return false;
#else
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return false;
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        if (fs::is_regular_file(fs::join(dir, exe))) return true;
    }
    return false;
#endif
}

// -----------------------------------------------------------------------------
//  同步执行
// -----------------------------------------------------------------------------

Result<CommandResult> run(const std::string& exe,
                          const std::vector<std::string>& args,
                          const std::string& working_dir,
                          int timeout_ms) {
    if (exe.empty()) {
        return Result<CommandResult>::fail(ErrorCode::InvalidArgument,
                                          "可执行文件路径为空", "process::run");
    }

    // 输出走临时文件而不是管道：管道填满会让子进程写阻塞、
    // 父进程又在等它结束，直接死锁。临时文件没有这个风险。
    const std::string out_path = fs::join(
        fs::is_directory(".") ? fs::absolute(".") : std::string{"."},
        ".penhu-proc-" + std::to_string(static_cast<unsigned>(std::rand())) + ".log");

#if defined(_WIN32)
    const std::wstring cmdline = build_command_line(exe, args);
    const std::wstring workdir = to_wide(working_dir);

    HANDLE out_handle = open_log_handle(out_path, /*append=*/false);

    auto spawned = spawn_raw(cmdline, workdir, out_handle, CREATE_SUSPENDED);

    if (out_handle != INVALID_HANDLE_VALUE) ::CloseHandle(out_handle);

    if (!spawned.ok) {
        fs::remove_file(out_path);
        return Result<CommandResult>::fail(
            ErrorCode::StorageFailure,
            "无法启动进程 '" + exe + "': " + spawned.error +
                "（检查路径、文件名与执行权限）",
            "process::run");
    }

    HANDLE job = attach_job(spawned.process);
    ::ResumeThread(spawned.thread);
    ::CloseHandle(spawned.thread);

    CommandResult result;
    bool timed_out = false;

    if (timeout_ms > 0) {
        if (::WaitForSingleObject(spawned.process, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
            timed_out = true;
            if (job != nullptr) {
                ::TerminateJobObject(job, 1);
            } else {
                ::TerminateProcess(spawned.process, 1);
            }
            ::WaitForSingleObject(spawned.process, 3000);
        }
    } else {
        ::WaitForSingleObject(spawned.process, INFINITE);
    }

    DWORD code = 0;
    ::GetExitCodeProcess(spawned.process, &code);
    result.exit_code = static_cast<int>(code);
    result.timed_out = timed_out;

    ::CloseHandle(spawned.process);
    if (job != nullptr) ::CloseHandle(job);

    const int64_t started_placeholder = 0;
    (void)started_placeholder;
    result.duration_ms = 0;
#else
    const int64_t started = now_ms();
    std::string cmd;
    cmd.reserve(exe.size() + 64);
    cmd += "\"" + exe + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    cmd += " > \"" + out_path + "\" 2>&1";

    const int rc = std::system(cmd.c_str());

    CommandResult result;
    if (rc == -1) {
        result.exit_code = -1;
    } else if (WIFEXITED(rc)) {
        result.exit_code = WEXITSTATUS(rc);
    } else {
        result.exit_code = -1;
    }
    result.duration_ms = now_ms() - started;
    (void)working_dir;
    (void)timeout_ms;
#endif

    {
        std::ifstream in(out_path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            result.output = ss.str();
        }
    }
    fs::remove_file(out_path);
    return Result<CommandResult>(std::move(result));
}

// -----------------------------------------------------------------------------
//  ChildProcess
// -----------------------------------------------------------------------------

ChildProcess::~ChildProcess() { release(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_),
      native_handle_(other.native_handle_),
      exit_code_(other.exit_code_),
      exited_(other.exited_) {
    other.pid_ = 0;
    other.native_handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        release();
        pid_ = other.pid_;
        native_handle_ = other.native_handle_;
        exit_code_ = other.exit_code_;
        exited_ = other.exited_;
        other.pid_ = 0;
        other.native_handle_ = nullptr;
    }
    return *this;
}

void ChildProcess::release() noexcept {
#if defined(_WIN32)
    if (native_handle_ != nullptr) {
        auto* impl = static_cast<NativeImpl*>(native_handle_);
        if (impl->job != nullptr) {
            // Job 配了 KILL_ON_JOB_CLOSE：关掉它等于把整棵进程树清掉。
            // 这是「析构不留孤儿」的保证。
            ::TerminateJobObject(impl->job, 0);
            ::CloseHandle(impl->job);
        } else if (impl->process != nullptr) {
            ::TerminateProcess(impl->process, 0);
        }
        if (impl->process != nullptr) ::CloseHandle(impl->process);
        delete impl;
        native_handle_ = nullptr;
    }
#else
    if (native_handle_ != nullptr) {
        auto* impl = static_cast<NativeImpl*>(native_handle_);
        if (impl->pid > 0) {
            // 析构不留孤儿 —— 这条保证在 POSIX 下更要紧：
            // 本地模型进程占着显存，进程被 kill 掉却不清理子进程的话，
            // 用户下次启动会看到「显存被占满但没有任何窗口」。
            // 但析构不该长时间阻塞，所以犹豫期只给 200ms。
            ::kill(-impl->pid, SIGTERM);
            int status = 0;
            for (int waited = 0; waited < 200; waited += 20) {
                const pid_t r = ::waitpid(impl->pid, &status, WNOHANG);
                if (r == impl->pid) break;
                if (r < 0 && errno != EINTR) break;
                ::usleep(20 * 1000);
            }
            ::kill(-impl->pid, SIGKILL);
            while (::waitpid(impl->pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        delete impl;
        native_handle_ = nullptr;
    }
#endif
    pid_ = 0;
}

Result<ChildProcess> ChildProcess::spawn(const std::string& exe,
                                         const std::vector<std::string>& args,
                                         const std::string& working_dir,
                                         const std::string& stdout_log_path) {
    if (exe.empty()) {
        return Result<ChildProcess>::fail(ErrorCode::InvalidArgument,
                                          "可执行文件路径为空", "spawn");
    }
    if (!fs::is_regular_file(exe) && !command_exists(exe)) {
        return Result<ChildProcess>::fail(
            ErrorCode::NotFound,
            "找不到可执行文件: " + exe + "（本地模型运行时可能尚未安装）", "spawn");
    }

#if defined(_WIN32)
    const std::wstring cmdline = build_command_line(exe, args);
    const std::wstring workdir = to_wide(working_dir);

    HANDLE out_handle = INVALID_HANDLE_VALUE;
    if (!stdout_log_path.empty()) {
        out_handle = open_log_handle(stdout_log_path, /*append=*/true);
    }

    auto spawned = spawn_raw(cmdline, workdir, out_handle, 0);
    if (out_handle != INVALID_HANDLE_VALUE) ::CloseHandle(out_handle);

    if (!spawned.ok) {
        return Result<ChildProcess>::fail(
            ErrorCode::StorageFailure,
            "启动失败 '" + exe + "': " + spawned.error, "spawn");
    }

    HANDLE job = attach_job(spawned.process);
    ::CloseHandle(spawned.thread);

    auto* impl = new NativeImpl{};
    impl->job = job;
    impl->process = spawned.process;

    ChildProcess proc;
    proc.pid_ = static_cast<int64_t>(spawned.pid);
    proc.native_handle_ = impl;
    proc.exit_code_ = -1;
    proc.exited_ = false;
    return Result<ChildProcess>(std::move(proc));
#else
    // ---- POSIX：fork + exec ----
    //
    // 不用 posix_spawn：需要在子进程里改工作目录，而它的 chdir 支持
    // （addchdir_np）是 glibc 扩展，fork+exec 没有任何可移植性顾虑。
    //
    // fork 之后到 exec 之前**只能调用 async-signal-safe 的函数**
    // （此刻进程里可能已经有别的线程、别的锁）。这里用到的是
    // setpgid / chdir / open / dup2 / close / execvp / _exit —— 全部满足。
    // argv 的字符串也在 fork **之前**就准备好：构造 std::string 不能放在子进程里。
    std::vector<std::string> argv_store;
    argv_store.reserve(args.size() + 1);
    argv_store.push_back(exe);
    for (const auto& a : args) argv_store.push_back(a);

    std::vector<char*> argv;
    argv.reserve(argv_store.size() + 1);
    for (auto& item : argv_store) argv.push_back(const_cast<char*>(item.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return Result<ChildProcess>::fail(
            ErrorCode::Internal,
            std::string("fork 失败: ") + std::strerror(errno), "spawn");
    }

    if (pid == 0) {
        // ================= 子进程 =================

        // 自立门户成新的进程组。llama-server 这类服务会再 fork 工作进程，
        // 终止时按进程组发信号才能把整棵树清掉 —— 只杀父进程的话，
        // 孙子进程会活下来继续占着显存，用户看到的是「关了还在跑」。
        ::setpgid(0, 0);

        if (!working_dir.empty()) {
            if (::chdir(working_dir.c_str()) != 0) ::_exit(126);
        }

        int out_fd = -1;
        if (!stdout_log_path.empty()) {
            out_fd = ::open(stdout_log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        }
        if (out_fd < 0) {
            // 不重定向的话，子进程的输出会和父进程（CLI 的输出）混在一起，
            // 那种「日志里突然多出几行 llama.cpp 的加载信息」就是这么来的。
            out_fd = ::open("/dev/null", O_WRONLY);
        }
        if (out_fd >= 0) {
            ::dup2(out_fd, STDOUT_FILENO);
            ::dup2(out_fd, STDERR_FILENO);
            if (out_fd > STDERR_FILENO) ::close(out_fd);
        }

        ::execvp(exe.c_str(), argv.data());
        ::_exit(127);   // exec 失败（程序不存在 / 没有执行权限）
    }

    // ================= 父进程 =================
    auto impl = std::make_unique<NativeImpl>();
    impl->pid = pid;

    ChildProcess proc;
    proc.pid_ = static_cast<int64_t>(pid);
    proc.native_handle_ = impl.release();
    proc.exit_code_ = -1;
    proc.exited_ = false;
    return Result<ChildProcess>(std::move(proc));
#endif
}

bool ChildProcess::is_running() const {
#if defined(_WIN32)
    if (pid_ == 0 || native_handle_ == nullptr) return false;
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->process == nullptr) return false;

    const DWORD wait = ::WaitForSingleObject(impl->process, 0);
    if (wait == WAIT_TIMEOUT) return true;
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(impl->process, &code);
        exit_code_ = static_cast<int>(code);
        exited_ = true;
        return false;
    }
    return false;
#else
    if (pid_ <= 0 || native_handle_ == nullptr) return false;
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->pid <= 0) return false;

    int status = 0;
    const pid_t r = ::waitpid(impl->pid, &status, WNOHANG);
    if (r == impl->pid) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        exited_ = true;
        impl->pid = -1;   // 已回收，别再 waitpid（第二次会 ECHILD）
        return false;
    }
    if (r == 0) return true;          // 还在跑
    if (errno == EINTR) return true;  // 被打断：下次再问
    return false;                     // ECHILD 等：没得等了
#endif
}

Status ChildProcess::terminate(int grace_ms) {
#if defined(_WIN32)
    if (pid_ == 0 || native_handle_ == nullptr) return status_ok();
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->process == nullptr) return status_ok();

    // 说明这个取舍：Windows 没有可用的「优雅终止」给控制台程序。
    // SendCtrlC 需要 AttachConsole，会改变本进程的控制台状态，副作用太大。
    // llama-server 是纯推理服务，没有需要落盘的持久状态，
    // 所以直接终止。如果以后换成有状态的进程，这里必须重做。
    (void)grace_ms;
    if (impl->job != nullptr) {
        ::TerminateJobObject(impl->job, 0);
    } else {
        ::TerminateProcess(impl->process, 0);
    }
    ::WaitForSingleObject(impl->process, 5000);

    DWORD code = 0;
    ::GetExitCodeProcess(impl->process, &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    return status_ok();
#else
    if (pid_ <= 0 || native_handle_ == nullptr) return status_ok();
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->pid <= 0) return status_ok();

    // 先给一个 SIGTERM 的犹豫期：llama-server 收到它能自己收尾
    // （释放显存、删掉临时文件）。信号发给**进程组**（负 pid），
    // 原因见 spawn 里的 setpgid。
    ::kill(-impl->pid, SIGTERM);

    int status = 0;
    const int budget = (grace_ms > 0) ? grace_ms : 1;
    for (int waited = 0; waited < budget; waited += 50) {
        const pid_t r = ::waitpid(impl->pid, &status, WNOHANG);
        if (r == impl->pid) {
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            exited_ = true;
            impl->pid = -1;
            return status_ok();
        }
        if (r < 0 && errno != EINTR) break;
        ::usleep(50 * 1000);
    }

    // 犹豫期过了还在：强杀整组，然后收尸（不等它的话会留下僵尸进程）
    ::kill(-impl->pid, SIGKILL);
    while (::waitpid(impl->pid, &status, 0) < 0 && errno == EINTR) {
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
    impl->pid = -1;
    return status_ok();
#endif
}

Status ChildProcess::kill() { return terminate(0); }

int ChildProcess::exit_code() const {
    if (!exited_) {
        is_running();   // 顺带尝试回收退出码
    }
    return exit_code_;
}

bool ChildProcess::wait(int timeout_ms) {
#if defined(_WIN32)
    if (pid_ == 0 || native_handle_ == nullptr) return true;
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->process == nullptr) return true;

    const DWORD wait = ::WaitForSingleObject(
        impl->process, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(impl->process, &code);
        exit_code_ = static_cast<int>(code);
        exited_ = true;
        return true;
    }
    return false;
#else
    if (pid_ <= 0 || native_handle_ == nullptr) return true;
    auto* impl = static_cast<NativeImpl*>(native_handle_);
    if (impl->pid <= 0) return true;   // 已经收过了，当作已结束

    int status = 0;
    if (timeout_ms < 0) {
        while (::waitpid(impl->pid, &status, 0) < 0 && errno == EINTR) {
        }
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        exited_ = true;
        impl->pid = -1;
        return true;
    }

    for (int waited = 0; waited <= timeout_ms; waited += 20) {
        const pid_t r = ::waitpid(impl->pid, &status, WNOHANG);
        if (r == impl->pid) {
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            exited_ = true;
            impl->pid = -1;
            return true;
        }
        if (r < 0 && errno != EINTR) return true;   // 收不到了，别死等
        ::usleep(20 * 1000);
    }
    return false;   // 超时，还在跑
#endif
}

}  // namespace penhu::process
