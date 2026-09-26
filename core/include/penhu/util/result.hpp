#pragma once
// =============================================================================
//  penhu/util/result.hpp
//  Result<T> 错误处理原语。
//
//  为什么不用异常 / std::expected：
//   * std::expected 是 C++23，MSVC 需要 /std:c++latest，可移植性差；
//   * 异常在 HTTP 请求边界上很难给出「稳定的错误码 + 人话描述」，
//     而前端要拿这两个东西渲染错误提示；
//   * 我要保证 crypto 层的密钥派生失败、LLM 网络失败都能被显式处理，
//     而不是漏到某个 catch(...) 里变成 500。
// =============================================================================

#include <string>
#include <variant>
#include <utility>
#include <type_traits>
#include <stdexcept>

namespace penhu {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,   // 入参不合法（金额为负、日期格式错、密码太短…）
    NotFound,          // 查不到（用户、记录、分类）
    AlreadyExists,     // 唯一约束冲突（用户名重复）
    AuthFailed,        // 认证失败（密码错、token 失效/过期）
    PermissionDenied,  // 越权（A 用户想读 B 用户的记录）
    StorageFailure,    // SQLite / 文件系统
    CryptoFailure,     // libsodium / SQLCipher 密钥错误
    NetworkFailure,    // 云端 LLM 不可达
    LlmFailure,        // 模型返回不可用内容
    ConfigError,       // 配置文件缺失或非法
    Internal
};

const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode   code{ErrorCode::Internal};
    std::string message;   // 给人看的描述
    std::string context;   // 给排查用的上下文（哪个函数、哪个 SQL、哪个 URL）

    std::string to_string() const;
};

/// 便捷构造
inline Error make_error(ErrorCode code, std::string message, std::string context = {}) {
    return Error{code, std::move(message), std::move(context)};
}

// -----------------------------------------------------------------------------
//  Result<T>
// -----------------------------------------------------------------------------
template <class T>
class Result {
public:
    using value_type = T;

    Result(T value) : data_(std::move(value)) {}          // NOLINT: 隐式转换是刻意设计
    Result(Error err) : data_(std::move(err)) {}          // NOLINT

    static Result ok(T v) { return Result(std::move(v)); }
    static Result fail(ErrorCode code, std::string msg, std::string ctx = {}) {
        return Result(Error{code, std::move(msg), std::move(ctx)});
    }

    bool is_ok() const noexcept { return std::holds_alternative<T>(data_); }
    bool is_err() const noexcept { return !is_ok(); }
    explicit operator bool() const noexcept { return is_ok(); }

    T&       value() &        { return std::get<T>(data_); }
    const T& value() const&   { return std::get<T>(data_); }
    T&&      value() &&       { return std::get<T>(std::move(data_)); }

    const Error& error() const { return std::get<Error>(data_); }

    T value_or(T fallback) const {
        return is_ok() ? value() : std::move(fallback);
    }

    /// 失败时抛异常——只在「绝不该失败」的场景用（比如构造期初始化）。
    T& expect(const char* what) & {
        if (is_err()) throw std::runtime_error(std::string(what) + ": " + error().to_string());
        return value();
    }

    /// 把内部错误换个更贴切的 code / 加上下文后继续上抛
    Result<T> with_context(std::string ctx) && {
        if (is_err()) {
            Error e = error();
            e.context = e.context.empty() ? std::move(ctx) : (e.context + " <- " + ctx);
            return Result<T>(std::move(e));
        }
        return Result<T>(std::move(value()));
    }

private:
    std::variant<T, Error> data_;
};

/// 无返回值的操作
using Status = Result<std::monostate>;

inline Status status_ok() { return Status(std::monostate{}); }
inline Status status_err(ErrorCode code, std::string msg, std::string ctx = {}) {
    return Status(Error{code, std::move(msg), std::move(ctx)});
}

/// 把 Result<T> 丢成一个 Status：只关心「成功 / 失败」，不要那个值。
///
/// 为什么需要它：Result<T> 刻意**不**做跨类型隐式转换（T 不同就不是同一个类型，
/// 隐式转换会让「把 Result<int> 当成 Result<string> 用」这种错误静默通过）。
/// 代价是数据库这类「调用返回 Result<StepResult>、而我自己只返回 Status」
/// 的地方会编译不过，得显式转换。这在 SQL 执行路径上很常见，所以给一个具名工具。
///
/// 参数用 const&，因为调用方往往在错误分支之后还要继续读这个对象
/// （例如 `st.value() == StepResult::Row`），不能把值 move 走。
template <class T>
Status to_status(const Result<T>& r) {
    return r.is_err() ? Status(r.error()) : status_ok();
}

// -----------------------------------------------------------------------------
//  控制流宏
// -----------------------------------------------------------------------------

/// 若 expr（返回 Status / Result<T>）失败，直接把错误上抛
#define PENHU_RETURN_IF_ERROR(expr)                       \
    do {                                                  \
        auto _penhu_st_ = (expr);                         \
        if (_penhu_st_.is_err()) return _penhu_st_.error(); \
    } while (0)

/// 若 expr 失败则上抛，否则把成功值 move 进新变量
///
/// ⚠ var **必须是裸标识符**，不能是 `out.field`、`arr[i]`、`p->field` 这类表达式。
/// 原因：下面用 `##var` 拼内部临时变量名，传成员访问会展开成
///     auto _penhu_res_out.categories = (...);
/// 这种一眼不像错、但编译器报「语法错误 / 重定义 / 类型包含 auto 的符号必须
/// 具有初始值设定项」的乱码（实测踩过，一次报出几十条错）。
/// 想给成员赋值就分两步：
///     PENHU_ASSIGN_OR_RETURN(tmp, ...);
///     obj.field = std::move(tmp);
#define PENHU_ASSIGN_OR_RETURN(var, expr)                          \
    auto _penhu_res_##var = (expr);                                \
    if (_penhu_res_##var.is_err()) return _penhu_res_##var.error(); \
    auto var = std::move(_penhu_res_##var).value()

/// 手动打错误日志并上抛（用于需要留痕的关键路径，例如密钥派生）
#define PENHU_RETURN_IF_ERROR_LOG(expr, logger)                      \
    do {                                                             \
        auto _penhu_st_ = (expr);                                    \
        if (_penhu_st_.is_err()) {                                   \
            (logger).error(_penhu_st_.error().to_string());           \
            return _penhu_st_.error();                                \
        }                                                            \
    } while (0)

}  // namespace penhu
