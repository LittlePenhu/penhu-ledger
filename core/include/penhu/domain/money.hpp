#pragma once
// =============================================================================
//  penhu/domain/money.hpp
//  金额类型：以「分」为最小单位的定点数。
//
//  为什么不用 double：
//    0.1 + 0.2 != 0.3。钱一旦用浮点存，日汇总会漂，月度对账会差几分，
//    而且这个误差在「按类别占比」里还会被放大成百分比的对不上。
//    所以内部一律 int64 分，只在展示层格式化成 "¥12.34"。
//
//  int64 分 = 约 9.2e16 分 = 9.2e14 元，够用到人类不需要记账为止。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include "penhu/util/result.hpp"

namespace penhu {

class Money {
public:
    constexpr Money() = default;
    constexpr explicit Money(int64_t minor_units) : minor_(minor_units) {}

    /// 从「分」构造
    static constexpr Money from_minor(int64_t minor) { return Money{minor}; }
    /// 从「元」构造（整数元）
    static constexpr Money from_yuan(int64_t yuan) { return Money{yuan * 100}; }

    /// 解析用户输入，接受 "12"、"12.3"、"12.34"、"¥12.34"、"1,234.56"、"-5"
    /// 超过 2 位小数会被拒（不做四舍五入，避免静默改掉用户的钱）
    static Result<Money> parse(std::string_view text);

    int64_t minor_units() const noexcept { return minor_; }

    Money& operator+=(const Money& o) noexcept { minor_ += o.minor_; return *this; }
    Money& operator-=(const Money& o) noexcept { minor_ -= o.minor_; return *this; }

    friend Money operator+(Money a, const Money& b) { return a += b; }
    friend Money operator-(Money a, const Money& b) { return a -= b; }
    friend Money operator-(const Money& a) { return Money{-a.minor_}; }

    friend bool operator==(const Money& a, const Money& b) noexcept { return a.minor_ == b.minor_; }
    friend bool operator!=(const Money& a, const Money& b) noexcept { return !(a == b); }
    friend bool operator< (const Money& a, const Money& b) noexcept { return a.minor_ <  b.minor_; }
    friend bool operator> (const Money& a, const Money& b) noexcept { return b <  a; }
    friend bool operator<=(const Money& a, const Money& b) noexcept { return !(b <  a); }
    friend bool operator>=(const Money& a, const Money& b) noexcept { return !(a <  b); }

    bool is_zero() const noexcept { return minor_ == 0; }
    bool is_negative() const noexcept { return minor_ < 0; }
    Money abs() const noexcept { return Money{minor_ < 0 ? -minor_ : minor_}; }

    /// 按比例取整（银行家舍入的简化版：四舍五入到分）。ratio 形如 0.23
    /// 注意：这是「展示/预算」用途；求和前先各自取整会引入误差，
    /// 所以统计模块一律先累加 minor 再取比例，绝不先取比例再累加。
    Money scaled(double ratio) const;

    /// "12.34"（不带货币符号）
    std::string to_plain_string() const;
    /// "¥12.34" / "-¥12.34"
    std::string to_display_string() const;
    /// 带千分位："¥1,234.56"
    std::string to_grouped_string() const;
    /// 供 JSON / DB 使用的稳定十进制字符串 "12.34"
    std::string serialize() const;

    /// 从 DB / JSON 反序列化，失败返回 nullopt
    static std::optional<Money> deserialize(std::string_view text);

private:
    int64_t minor_{0};
};

/// 求和辅助（避免调用方手写循环）
int64_t sum_minor(const int64_t* begin, const int64_t* end);

}  // namespace penhu
