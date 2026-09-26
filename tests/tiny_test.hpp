#pragma once
// =============================================================================
//  tests/tiny_test.hpp
//  极简断言框架（header-only）。
//
//  为什么不引 Catch2 / GoogleTest：
//    两个都要走 vcpkg 再编一遍，而本项目需要的全部功能就是
//    「注册用例 + 跑 + 数失败数 + 打印哪一行挂了」，不到 150 行。
//
//  一个必须知道的坑（踩过）：
//    预处理器的宏参数只按**圆括号**判断嵌套层级，花括号不保护逗号。
//    所以 TT_EQ(Date{2026, 9, 18}.x(), ...) 会被拆成 4 个参数，
//    报出来的错是「应为 } 而不是 )」——完全看不出原因。
//    对策有两层：
//      1) TT_CHECK / TT_IS_ERR / TT_IS_OK 写成可变参数宏，内部用 (__VA_ARGS__)
//         包一层，把逗号保护住；
//      2) TT_EQ 这种必须拆两个参数的，调用方要用 tt::d(y,m,d) 这种函数形式
//         来构造日期，而不是 Date{y,m,d}。
// =============================================================================

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "penhu/domain/date.hpp"

namespace tt {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> instance;
    return instance;
}

inline int& failure_count() { static int n = 0; return n; }
inline int& check_count()   { static int n = 0; return n; }
inline int& case_failures() { static int n = 0; return n; }
inline std::string& current_case() { static std::string s; return s; }

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    ++case_failures();
    std::fprintf(stderr, "    [FAIL] %s:%d  %s\n", file, line, message.c_str());
    std::fflush(stderr);
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        cases().push_back(Case{suite, name, std::move(fn)});
    }
};

inline int run_all() {
    int passed_cases = 0;
    int failed_cases = 0;
    std::string last_suite;

    for (const auto& c : cases()) {
        if (c.suite != last_suite) {
            std::printf("\n== %s ==\n", c.suite.c_str());
            std::fflush(stdout);
            last_suite = c.suite;
        }
        current_case() = c.suite + " / " + c.name;
        case_failures() = 0;
        std::printf("  . %s\n", c.name.c_str());
        std::fflush(stdout);

        try {
            c.fn();
        } catch (const std::exception& e) {
            report_failure(__FILE__, __LINE__,
                           std::string("抛出未捕获异常: ") + e.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "抛出未知异常");
        }

        if (case_failures() == 0) {
            ++passed_cases;
        } else {
            ++failed_cases;
        }
    }

    std::printf("\n---------------------------------------------\n");
    std::printf("用例: %d 通过 / %d 失败（共 %d）\n",
                passed_cases, failed_cases, passed_cases + failed_cases);
    std::printf("断言: %d 条，失败 %d 条\n", check_count(), failure_count());
    std::printf("---------------------------------------------\n");
    return failed_cases == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
//  测试辅助：日期构造（见文件头说明的坑）
// -----------------------------------------------------------------------------
inline penhu::Date d(int year, int month, int day) {
    return penhu::Date{year, static_cast<unsigned>(month), static_cast<unsigned>(day)};
}

inline penhu::DateRange dr(const penhu::Date& from, const penhu::Date& to) {
    return penhu::DateRange{from, to};
}

/// 元 -> 分，让测试里的金额写法接近真实数字
inline int64_t yuan(int64_t major) { return major * 100; }

}  // namespace tt

// -----------------------------------------------------------------------------
//  宏
// -----------------------------------------------------------------------------

// 函数名用 __LINE__ 而不是把用例名拼进标识符：
// 用例名是给人看的自由文本（中文、带空格、带数字），拼成 C++ 标识符会直接语法错。
//
// 注意 TT_CONCAT 必须分两层：直接写 a##__LINE__ 会把 __LINE__ 当成字面文本粘上去
// （结果是 tt_case___LINE__ 这种），多一层宏调用才会先展开再粘。
#define TT_CONCAT_INNER(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT_INNER(a, b)

#define TT_TEST(suite, name)                                                      \
    static void TT_CONCAT(tt_case_, __LINE__)();                                  \
    static ::tt::Registrar TT_CONCAT(tt_reg_, __LINE__)(                          \
        #suite, #name, TT_CONCAT(tt_case_, __LINE__));                            \
    static void TT_CONCAT(tt_case_, __LINE__)()

#define TT_CHECK(...)                                                             \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        if (!(__VA_ARGS__)) {                                                     \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "条件不成立: " #__VA_ARGS__);                    \
        }                                                                         \
    } while (0)

#define TT_CHECK_MSG(cond, msg)                                                   \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        if (!(cond)) {                                                            \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "条件不成立: " #cond " | " + std::string(msg));   \
        }                                                                         \
    } while (0)

#define TT_EQ(actual, expected)                                                   \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        const auto& _a = (actual);                                                \
        const auto& _e = (expected);                                              \
        if (!(_a == _e)) {                                                        \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 std::string("期望 " #actual " == " #expected) +  \
                                     "，实际不等");                                \
        }                                                                         \
    } while (0)

#define TT_EQ_MSG(actual, expected, msg)                                          \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        const auto& _a = (actual);                                                \
        const auto& _e = (expected);                                              \
        if (!(_a == _e)) {                                                        \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 std::string("期望 " #actual " == " #expected) +  \
                                     " | " + std::string(msg));                   \
        }                                                                         \
    } while (0)

#define TT_NEAR(actual, expected, eps)                                            \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        const double _a = static_cast<double>(actual);                            \
        const double _e = static_cast<double>(expected);                          \
        if (!(std::fabs(_a - _e) <= (eps))) {                                     \
            char _buf[200];                                                       \
            std::snprintf(_buf, sizeof(_buf),                                     \
                          "期望 %s 约等于 %s，实际 %.6f vs %.6f", #actual,          \
                          #expected, _a, _e);                                     \
            ::tt::report_failure(__FILE__, __LINE__, _buf);                       \
        }                                                                         \
    } while (0)

#define TT_FAIL(msg) ::tt::report_failure(__FILE__, __LINE__, msg)

#define TT_IS_ERR(...)                                                            \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        auto&& _r = (__VA_ARGS__);                                                \
        if (_r.is_ok()) {                                                         \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "期望失败，但它成功了: " #__VA_ARGS__);            \
        }                                                                         \
    } while (0)

// 参数名不能叫 code：宏体会把参数在**所有**标识符位置替换掉，
// 包括 `_r.error().code` 里那个成员名 —— 于是 `.code` 变成
// `.ErrorCode::InvalidArgument`，GCC 直接报「不是 Error 的成员」。
// MSVC 的传统预处理器在 `.` 之后不做替换（不符合标准），所以这个 bug
// 在 Windows 上一直没暴露，直到 Linux 侧第一次编译。
// 结论：宏参数名要避开宏体里出现的任何成员名/变量名。
#define TT_IS_ERR_CODE(expr, expected_code)                                       \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        auto&& _r = (expr);                                                       \
        if (_r.is_ok()) {                                                         \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "期望 " #expr " 失败，但它成功了");                 \
        } else if (_r.error().code != (expected_code)) {                          \
            ::tt::report_failure(                                                 \
                __FILE__, __LINE__,                                               \
                "错误码不符: 期望 " + std::string(#expected_code) + "，实际 " +    \
                    std::string(::penhu::to_string(_r.error().code)) + " | " +    \
                    _r.error().message);                                          \
        }                                                                         \
    } while (0)

#define TT_IS_OK(...)                                                             \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        auto&& _r = (__VA_ARGS__);                                                \
        if (_r.is_err()) {                                                        \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "期望成功，但失败了: " + _r.error().to_string() +  \
                                     " | " #__VA_ARGS__);                         \
        }                                                                         \
    } while (0)

/// 期望成功；失败则打印错误并立刻返回当前测试函数。
/// 测试函数返回 void，所以不能用 PENHU_ASSIGN_OR_RETURN（那个 return 的是 Error）。
#define TT_REQUIRE_OK(...)                                                        \
    do {                                                                          \
        ++::tt::check_count();                                                    \
        auto&& _r = (__VA_ARGS__);                                                \
        if (_r.is_err()) {                                                        \
            ::tt::report_failure(__FILE__, __LINE__,                              \
                                 "前置条件失败，提前结束本用例: " +                    \
                                     _r.error().to_string());                      \
            return;                                                               \
        }                                                                         \
    } while (0)

/// 取值；失败则报错并返回
#define TT_TRY_ASSIGN(var, ...)                                                   \
    auto _tt_res_##var = (__VA_ARGS__);                                           \
    if (_tt_res_##var.is_err()) {                                                 \
        ::tt::report_failure(__FILE__, __LINE__,                                  \
                             "取值失败，提前结束本用例: " +                          \
                                 _tt_res_##var.error().to_string());              \
        return;                                                                   \
    }                                                                             \
    auto var = std::move(_tt_res_##var).value()
