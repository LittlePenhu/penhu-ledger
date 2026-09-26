#include "penhu/domain/money.hpp"

#include <cstdio>
#include <cmath>
#include <limits>
#include <algorithm>

namespace penhu {
namespace {

constexpr int64_t kMinorPerMajor = 100;
constexpr int64_t kMaxMinor = std::numeric_limits<int64_t>::max();

std::string_view trim(std::string_view s) {
    const auto not_space = [](char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    };
    while (!s.empty() && !not_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && !not_space(s.back()))  s.remove_suffix(1);
    return s;
}

/// 千分位分组：把 "1234567" -> "1,234,567"
std::string group_digits(const std::string& digits) {
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const size_t n = digits.size();
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace

Result<Money> Money::parse(std::string_view text) {
    std::string_view s = trim(text);
    if (s.empty()) {
        return Result<Money>::fail(ErrorCode::InvalidArgument, "金额不能为空", "Money::parse");
    }

    // 去掉货币符号前缀（含全角￥）
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.front());
        if (c == '$' || c == 0xA3 /*£*/) { s.remove_prefix(1); continue; }
        // UTF-8 的 ¥ (U+00A5) = C2 A5, ￥ (U+FFE5) = EF BF A5, € = E2 82 AC
        if (s.size() >= 2 && c == 0xC2 && static_cast<unsigned char>(s[1]) == 0xA5) {
            s.remove_prefix(2); continue;
        }
        if (s.size() >= 3 && c == 0xEF && static_cast<unsigned char>(s[1]) == 0xBF &&
            static_cast<unsigned char>(s[2]) == 0xA5) {
            s.remove_prefix(3); continue;
        }
        if (s.size() >= 3 && c == 0xE2 && static_cast<unsigned char>(s[1]) == 0x82 &&
            static_cast<unsigned char>(s[2]) == 0xAC) {
            s.remove_prefix(3); continue;
        }
        break;
    }
    s = trim(s);
    if (s.empty()) {
        return Result<Money>::fail(ErrorCode::InvalidArgument, "金额只有货币符号，缺少数字",
                                   "Money::parse");
    }

    bool negative = false;
    if (s.front() == '+' || s.front() == '-') {
        negative = (s.front() == '-');
        s.remove_prefix(1);
    }

    std::string int_digits;
    std::string frac_digits;
    bool seen_dot = false;
    bool seen_digit = false;

    // 用下标循环而不是范围 for：中点 U+00B7 在 UTF-8 里是**两个字节** C2 B7，
    // 要认它就得能往前多吃一个字节。
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ',' || c == '_') continue;              // 允许千分位

        // 小数点：ASCII 的 '.'，或中点 U+00B7（输入法手滑打出来的「·」）。
        //
        // 这里原来写的是 `c == 0xB7`，它**恒为假** —— char 是带符号的
        // （MSVC 和 GCC 都是），0xB7 = 183 超出正数范围，编译器甚至直接警告
        // 「comparison is always false」。也就是说「中点也认」这条从来没生效过。
        // 而且就算把 char 换成无符号，只比 B7 也不够：UTF-8 里它前面还有一个 C2，
        // 会掉进「非法字符」分支。所以按多字节序列判断。
        bool is_dot = (c == '.');
        if (!is_dot && c == 0xC2 && i + 1 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) == 0xB7) {
            is_dot = true;
            ++i;                                          // 连第二个字节一起吃掉
        }
        if (is_dot) {
            if (seen_dot) {
                return Result<Money>::fail(ErrorCode::InvalidArgument,
                                           "金额里有多个小数点: " + std::string(text),
                                           "Money::parse");
            }
            seen_dot = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return Result<Money>::fail(ErrorCode::InvalidArgument,
                                       std::string("金额含非法字符 '") + s[i] + "': " + std::string(text),
                                       "Money::parse");
        }
        seen_digit = true;
        (seen_dot ? frac_digits : int_digits).push_back(static_cast<char>(c));
    }

    if (!seen_digit) {
        return Result<Money>::fail(ErrorCode::InvalidArgument,
                                   "金额缺少数字: " + std::string(text), "Money::parse");
    }
    if (frac_digits.size() > 2) {
        // 刻意不四舍五入：静默改掉用户输入的金额是记账软件最不可接受的 bug
        return Result<Money>::fail(
            ErrorCode::InvalidArgument,
            "金额最多保留 2 位小数（收到 " + std::to_string(frac_digits.size()) + " 位）: " +
                std::string(text),
            "Money::parse");
    }
    while (frac_digits.size() < 2) frac_digits.push_back('0');

    // 整数部分：允许 "0"、允许空（".5" 这种）
    if (int_digits.empty()) int_digits = "0";

    int64_t minor = 0;
    for (char c : int_digits) {
        const int64_t d = c - '0';
        if (minor > (kMaxMinor - d) / 10) {
            return Result<Money>::fail(ErrorCode::InvalidArgument,
                                       "金额超出可表示范围: " + std::string(text), "Money::parse");
        }
        minor = minor * 10 + d;
    }

    int64_t frac = 0;
    for (char c : frac_digits) frac = frac * 10 + (c - '0');

    // 溢出检查必须用**实际的小数部分**，不能用 99 做保守估计。
    // 用 99 的话 (INT64_MAX-99)/100 会比真实上界小 1，
    // 于是 "92233720368547758.07"（正好等于 INT64_MAX 分）会被误判为溢出。
    // 这个 bug 是靠单元测试里那条边界用例抓出来的。
    if (minor > (kMaxMinor - frac) / kMinorPerMajor) {
        return Result<Money>::fail(ErrorCode::InvalidArgument,
                                   "金额超出可表示范围: " + std::string(text), "Money::parse");
    }
    minor = minor * kMinorPerMajor + frac;

    return Result<Money>(Money::from_minor(negative ? -minor : minor));
}

std::string Money::to_plain_string() const {
    const bool neg = minor_ < 0;
    // 注意 INT64_MIN 取负会溢出，所以先转成无符号再取绝对值
    const uint64_t mag = neg
        ? (~static_cast<uint64_t>(minor_) + 1ULL)
        : static_cast<uint64_t>(minor_);

    const uint64_t major = mag / 100ULL;
    const uint64_t frac  = mag % 100ULL;

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s%llu.%02llu",
                  neg ? "-" : "",
                  static_cast<unsigned long long>(major),
                  static_cast<unsigned long long>(frac));
    return std::string(buf);
}

std::string Money::serialize() const {
    return to_plain_string();
}

std::string Money::to_display_string() const {
    return "¥" + to_plain_string();
}

std::string Money::to_grouped_string() const {
    const bool neg = minor_ < 0;
    const uint64_t mag = neg
        ? (~static_cast<uint64_t>(minor_) + 1ULL)
        : static_cast<uint64_t>(minor_);

    const std::string major = std::to_string(mag / 100ULL);
    const uint64_t frac = mag % 100ULL;

    char fb[8];
    std::snprintf(fb, sizeof(fb), "%02llu", static_cast<unsigned long long>(frac));

    std::string out = "¥";
    if (neg) out += '-';
    out += group_digits(major);
    out += '.';
    out += fb;
    return out;
}

Money Money::scaled(double ratio) const {
    if (!std::isfinite(ratio)) return Money{};
    const double v = static_cast<double>(minor_) * ratio;
    if (v >= static_cast<double>(kMaxMinor)) return Money{kMaxMinor};
    if (v <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
        return Money{std::numeric_limits<int64_t>::min()};
    }
    return Money{static_cast<int64_t>(std::llround(v))};
}

std::optional<Money> Money::deserialize(std::string_view text) {
    auto r = parse(text);
    if (r.is_err()) return std::nullopt;
    return r.value();
}

int64_t sum_minor(const int64_t* begin, const int64_t* end) {
    int64_t total = 0;
    for (const int64_t* p = begin; p != end; ++p) total += *p;
    return total;
}

}  // namespace penhu
