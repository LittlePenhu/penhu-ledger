#include "penhu/domain/date.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace penhu {
namespace {

// ---------------------------------------------------------------------------
//  Howard Hinnant 的 civil-days 算法：日历日 <-> 1970-01-01 起的天数
//  常数时间、无查表、公元前后的边界都正确。选它是因为我们的日期区间运算
//  （补零、滑动窗口、3σ 比较）全部建立在这个偏移量上，它必须绝对可靠。
// ---------------------------------------------------------------------------
constexpr int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= (m <= 2u) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;  // [0, 365]
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;          // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;  // [0, 399]
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);        // [0, 365]
    const unsigned mp  = (5u * doy + 2u) / 153u;                            // [0, 11]
    d = doy - (153u * mp + 2u) / 5u + 1u;                                   // [1, 31]
    m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    y += (m <= 2u) ? 1 : 0;
}

constexpr bool leap_year(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

constexpr unsigned dim_of(int y, unsigned m) noexcept {
    // 平年每月天数；2 月由 leap_year 决定
    constexpr std::array<unsigned, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1u || m > 12u) return 0u;
    if (m == 2u) return leap_year(y) ? 29u : 28u;
    return kDays[m - 1u];
}

std::string_view trim(std::string_view s) {
    const auto ns = [](char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    while (!s.empty() && !ns(s.front())) s.remove_prefix(1);
    while (!s.empty() && !ns(s.back()))  s.remove_suffix(1);
    return s;
}

std::tm to_tm_local(std::time_t t) noexcept {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::tm to_tm_utc(std::time_t t) noexcept {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

/// 把「拆解后的时间」当作 UTC 解释，得到 epoch 秒。
/// 用它来算时区偏移：同一个 time_t 拆成本地/UTC 两套，各自按 UTC 重算，
/// 差值就是本地偏移。比直接读 tm_gmtoff 更可移植（MSVC 没有 tm_gmtoff）。
int64_t tm_as_utc(const std::tm& tm) noexcept {
    const int64_t days = days_from_civil(tm.tm_year + 1900,
                                         static_cast<unsigned>(tm.tm_mon + 1),
                                         static_cast<unsigned>(tm.tm_mday));
    return days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Date
// -----------------------------------------------------------------------------

bool Date::is_valid() const noexcept {
    if (year < 1 || year > 9999) return false;
    if (month < 1u || month > 12u) return false;
    if (day < 1u || day > dim_of(year, month)) return false;
    return true;
}

int64_t Date::to_epoch_days() const noexcept {
    return days_from_civil(year, month, day);
}

Date Date::from_epoch_days(int64_t days) noexcept {
    Date d;
    civil_from_days(days, d.year, d.month, d.day);
    return d;
}

Result<Date> Date::create(int y, int m, int d) {
    Date out{y, static_cast<unsigned>(m), static_cast<unsigned>(d)};
    if (!out.is_valid()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "非法日期 %04d-%02d-%02d", y, m, d);
        return Result<Date>::fail(ErrorCode::InvalidArgument, buf, "Date::create");
    }
    return Result<Date>(out);
}

Result<Date> Date::parse(std::string_view text) {
    const std::string_view raw = text;
    std::string_view s = trim(text);
    if (s.empty()) {
        return Result<Date>::fail(ErrorCode::InvalidArgument, "日期不能为空", "Date::parse");
    }

    std::vector<std::string> groups;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur.push_back(c);
        } else if (c == '-' || c == '/' || c == '.' || c == ' ') {
            if (!cur.empty()) { groups.push_back(cur); cur.clear(); }
        } else {
            return Result<Date>::fail(
                ErrorCode::InvalidArgument,
                std::string("日期含非法字符 '") + c + "': " + std::string(raw), "Date::parse");
        }
    }
    if (!cur.empty()) groups.push_back(cur);

    int y = 0, m = 0, d = 0;
    if (groups.size() == 3) {
        // 年份必须是 4 位。"26-09-18" 这种不能猜——是 2026 还是 1926？
        // 猜错的后果是账记到了错误的一百年里，而且用户不会立刻发现。
        if (groups[0].size() != 4) {
            return Result<Date>::fail(
                ErrorCode::InvalidArgument,
                "年份必须是 4 位数字（收到 '" + groups[0] + "'，无法判断是 20" + groups[0] +
                    " 还是 19" + groups[0] + "）",
                "Date::parse");
        }
        y = std::atoi(groups[0].c_str());
        m = std::atoi(groups[1].c_str());
        d = std::atoi(groups[2].c_str());
    } else if (groups.size() == 1 && groups[0].size() == 8) {
        y = std::atoi(groups[0].substr(0, 4).c_str());
        m = std::atoi(groups[0].substr(4, 2).c_str());
        d = std::atoi(groups[0].substr(6, 2).c_str());
    } else {
        return Result<Date>::fail(
            ErrorCode::InvalidArgument,
            "无法识别的日期格式（期望 YYYY-MM-DD）: " + std::string(raw), "Date::parse");
    }

    auto r = Date::create(y, m, d);
    if (r.is_err()) return r;
    return r;
}

Date Date::today() {
    return timeutil::to_local_date(timeutil::now_epoch_seconds());
}

Date Date::from_epoch_seconds(int64_t seconds_utc_local) {
    return timeutil::to_local_date(seconds_utc_local);
}

std::string Date::to_string() const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", year, month, day);
    return std::string(buf);
}

std::string Date::to_compact() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02u", year, month, day);
    return std::string(buf);
}

int Date::day_of_week() const noexcept {
    // 1970-01-01 是周四；(days + 4) mod 7 后 0=周日
    const int64_t d = to_epoch_days();
    return static_cast<int>(((d + 4) % 7 + 7) % 7);
}

std::string Date::weekday_zh() const {
    static const std::array<const char*, 7> kNames{"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return kNames[static_cast<size_t>(day_of_week())];
}

bool Date::is_weekend() const noexcept {
    const int w = day_of_week();
    return w == 0 || w == 6;
}

Date Date::add_days(int n) const noexcept {
    return from_epoch_days(to_epoch_days() + n);
}

Date Date::add_months(int n) const {
    // 用「自 0 年 1 月起的月序号」做加减，再夹回目标月的实际天数。
    // 不夹的话 1/31 + 1 个月会得到 2/31 这种不存在的东西。
    const int64_t total = static_cast<int64_t>(year) * 12 + static_cast<int64_t>(month) - 1 + n;
    int64_t y = total / 12;
    int64_t m = total % 12;
    if (m < 0) { m += 12; y -= 1; }

    const unsigned ny = static_cast<unsigned>(y);
    const unsigned nm = static_cast<unsigned>(m + 1);
    const unsigned nd = std::min(day, dim_of(static_cast<int>(ny), nm));
    return Date{static_cast<int>(ny), nm, nd};
}

Date Date::first_day_of_month() const { return Date{year, month, 1u}; }

Date Date::last_day_of_month() const { return Date{year, month, dim_of(year, month)}; }

int Date::iso_week() const noexcept {
    // ISO 8601：周一为一周之始，包含周四的那一周归属该年
    const int64_t ed = to_epoch_days();
    const int wd_mon0 = static_cast<int>(((ed + 3) % 7 + 7) % 7);   // 0=周一
    const int64_t thursday = ed - wd_mon0 + 3;
    const Date th = from_epoch_days(thursday);
    const int64_t jan1 = days_from_civil(static_cast<int>(th.year), 1u, 1u);
    const int doy = static_cast<int>(thursday - jan1) + 1;
    return (doy - 1) / 7 + 1;
}

Date Date::week_start() const noexcept {
    return add_days(-(((day_of_week() + 6) % 7)));   // 回退到本周一
}

int  Date::days_in_month() const noexcept { return static_cast<int>(dim_of(year, month)); }
bool Date::is_leap_year() const noexcept { return leap_year(year); }

bool Date::same_month_as(const Date& o) const noexcept {
    return year == o.year && month == o.month;
}

bool Date::same_week_as(const Date& o) const noexcept {
    return week_start() == o.week_start();
}

std::vector<Date> date_range_inclusive(const Date& from, const Date& to) {
    std::vector<Date> out;
    if (!from.is_valid() || !to.is_valid() || to < from) return out;

    const int64_t span = to.to_epoch_days() - from.to_epoch_days();
    // 上限保护：避免有人传 1900-01-01 到 2100-01-01 直接把内存吃光
    constexpr int64_t kMaxSpanDays = 366 * 20;
    if (span > kMaxSpanDays) return out;

    out.reserve(static_cast<size_t>(span) + 1);
    for (int64_t i = 0; i <= span; ++i) {
        out.push_back(Date::from_epoch_days(from.to_epoch_days() + i));
    }
    return out;
}

DateRange month_range(const Date& any) {
    return DateRange{any.first_day_of_month(), any.last_day_of_month()};
}

DateRange week_range(const Date& any) {
    const Date s = any.week_start();
    return DateRange{s, s.add_days(6)};
}

DateRange year_range(const Date& any) {
    return DateRange{Date{any.year, 1u, 1u}, Date{any.year, 12u, 31u}};
}

DateRange last_n_days(const Date& end_inclusive, int n) {
    if (n < 1) n = 1;
    return DateRange{end_inclusive.add_days(-(n - 1)), end_inclusive};
}

// -----------------------------------------------------------------------------
//  timeutil
// -----------------------------------------------------------------------------
namespace timeutil {

int64_t now_epoch_seconds() noexcept {
    using namespace std::chrono;
    return static_cast<int64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

int32_t local_utc_offset_seconds() noexcept {
    const std::time_t now = std::time(nullptr);
    const std::tm lt = to_tm_local(now);
    const std::tm gt = to_tm_utc(now);
    return static_cast<int32_t>(tm_as_utc(lt) - tm_as_utc(gt));
}

Date to_local_date(int64_t epoch_seconds) noexcept {
    const int64_t shifted = epoch_seconds + local_utc_offset_seconds();
    // 向下取整的整数除法（epoch 为负时 C++ 的 / 是向零取整，必须手动修正）
    int64_t days = shifted / 86400;
    if (shifted % 86400 != 0 && shifted < 0) days -= 1;
    return Date::from_epoch_days(days);
}

int64_t local_date_start_epoch(const Date& d) noexcept {
    return d.to_epoch_days() * 86400 - local_utc_offset_seconds();
}

std::string to_iso8601_utc(int64_t epoch_seconds) {
    const std::tm tm = to_tm_utc(static_cast<std::time_t>(epoch_seconds));
    // 缓冲区给 80 而不是 32：GCC 的 -Wformat-truncation 会算 `%04d` 的**理论上界**
    // （tm_year 是 int，加 1900 后最坏 11 位），32 字节在它眼里不够。
    // 实际年份当然到不了那么大，但既然只是几个字节的栈空间，就别留下这个警告噪音 ——
    // 警告一多，真问题就淹了。
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

std::string to_local_string(int64_t epoch_seconds) {
    const std::tm tm = to_tm_local(static_cast<std::time_t>(epoch_seconds));
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

}  // namespace timeutil

}  // namespace penhu
