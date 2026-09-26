#pragma once
// =============================================================================
//  penhu/domain/date.hpp
//  日历日期（不含时间）与时间戳工具。
//
//  为什么不直接用 std::chrono::year_month_day：
//   * std::chrono::parse 在 MSVC 上的支持是分版本的，17.4 之前基本没有，
//     而我们需要稳定地把 "2026-09-18" 这类字符串读进来；
//   * 记账的日期语义是「用户本地日历日」，不是 UTC 时刻。把它绑到
//     system_clock 上反而要处处担心时区换算把 23:30 的账算到第二天。
//   * 网页端移植时，同一套日历算法用 JS 重写也就 30 行，语义完全一致。
//
//  实现用的是 Howard Hinnant 的 civil-days 算法（days_from_civil /
//  civil_from_days），常数时间、无查表、1970 年前后都正确。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include "penhu/util/result.hpp"

namespace penhu {

/// 纯日历日期，值语义，默认无效
struct Date {
    int year{0};
    unsigned month{0};
    unsigned day{0};

    bool is_valid() const noexcept;

    /// 距 1970-01-01 的天数（可为负）
    int64_t to_epoch_days() const noexcept;
    static Date from_epoch_days(int64_t days) noexcept;

    static Result<Date> parse(std::string_view text);          // "2026-09-18" / "2026/9/8"
    static Result<Date> create(int y, int m, int d);           // 带校验
    static Date today();                                       // 本地日历日
    static Date from_epoch_seconds(int64_t seconds_utc_local);

    std::string to_string() const;      // "2026-09-18"（已补零，可直接进 SQL 比较）
    std::string to_compact() const;     // "20260918"

    /// 0=周日 … 6=周六
    int day_of_week() const noexcept;
    /// "周一" … "周日"
    std::string weekday_zh() const;

    bool is_weekend() const noexcept;

    Date add_days(int n) const noexcept;
    Date add_months(int n) const;      // 处理 1/31 + 1 月 = 2/28 这类情况
    Date first_day_of_month() const;
    Date last_day_of_month() const;

    /// ISO 周编号（周一为一周之始）
    int iso_week() const noexcept;
    /// 该 ISO 周的周一
    Date week_start() const noexcept;

    int  days_in_month() const noexcept;
    bool is_leap_year() const noexcept;

    /// 同一月份 / 同一 ISO 周
    bool same_month_as(const Date& o) const noexcept;
    bool same_week_as(const Date& o) const noexcept;

    friend bool operator==(const Date& a, const Date& b) noexcept {
        return a.year == b.year && a.month == b.month && a.day == b.day;
    }
    friend bool operator!=(const Date& a, const Date& b) noexcept { return !(a == b); }
    friend bool operator< (const Date& a, const Date& b) noexcept {
        if (a.year != b.year) return a.year < b.year;
        if (a.month != b.month) return a.month < b.month;
        return a.day < b.day;
    }
    friend bool operator> (const Date& a, const Date& b) noexcept { return b < a; }
    friend bool operator<=(const Date& a, const Date& b) noexcept { return !(b < a); }
    friend bool operator>=(const Date& a, const Date& b) noexcept { return !(a < b); }
};

/// 生成 [from, to] 之间的每一天（含端点），按升序。
/// 统计模块要靠它把「没有记账的日期」补成 0，否则趋势图会骗人。
std::vector<Date> date_range_inclusive(const Date& from, const Date& to);

/// 自然月 / 自然周 / 自然年的边界
struct DateRange {
    Date from;
    Date to;
    bool contains(const Date& d) const noexcept { return !(d < from) && !(d > to); }
};

DateRange month_range(const Date& any_day_in_month);
DateRange week_range(const Date& any_day_in_week);
DateRange year_range(const Date& any_day_in_year);
DateRange last_n_days(const Date& end_inclusive, int n);

// -----------------------------------------------------------------------------
//  时间戳工具（统一用 UTC epoch 秒，展示层再转本地）
// -----------------------------------------------------------------------------
namespace timeutil {

/// 当前 UTC epoch 秒
int64_t now_epoch_seconds() noexcept;

/// 当前本地时区相对 UTC 的偏移秒数（东八区 = 28800）
int32_t local_utc_offset_seconds() noexcept;

/// 把 UTC epoch 秒转成本地日历日
Date to_local_date(int64_t epoch_seconds) noexcept;

/// 本地日历日的 00:00 对应的 UTC epoch 秒
int64_t local_date_start_epoch(const Date& d) noexcept;

/// ISO 8601 UTC 字符串 "2026-09-18T08:35:00Z"
std::string to_iso8601_utc(int64_t epoch_seconds);

/// 本地可读 "2026-09-18 16:35:00"
std::string to_local_string(int64_t epoch_seconds);

}  // namespace timeutil

}  // namespace penhu
