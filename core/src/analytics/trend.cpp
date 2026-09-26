#include "penhu/analytics/trend.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace penhu::analytics {
namespace {

/// 数据够不够支撑趋势结论的阈值。
/// 7 天是滑动平均窗口的两倍多一点，低于这个数移动平均基本就是原值本身，
/// 画出来会让人误以为「消费很平稳」。
constexpr int64_t kMinDaysForTrend = 14;

std::vector<TrendPoint> build_points(const std::vector<int64_t>& daily_values,
                                     const std::vector<Date>& dates,
                                     int window) {
    std::vector<TrendPoint> points;
    points.reserve(daily_values.size());

    const int64_t n = static_cast<int64_t>(daily_values.size());
    for (int64_t i = 0; i < n; ++i) {
        TrendPoint p;
        p.date = dates[static_cast<size_t>(i)];
        p.value_minor = daily_values[static_cast<size_t>(i)];

        // 居中滑动平均：窗口 [i-half, i+half]，边界处自动收缩。
        // 用居中而不是滞后窗口，是因为滞后窗口会让趋势线整体右移，
        // 看起来像「今天的花费影响了昨天的平均」——反直觉。
        const int64_t half = window / 2;
        const int64_t from = std::max<int64_t>(0, i - half);
        const int64_t to   = std::min<int64_t>(n - 1, i + half);

        int64_t sum = 0;
        int count = 0;
        for (int64_t k = from; k <= to; ++k) {
            sum += daily_values[static_cast<size_t>(k)];
            ++count;
        }
        p.moving_average_minor = count > 0 ? sum / count : 0;
        p.window_used = count;
        points.push_back(p);
    }
    return points;
}

PeriodComparison compare_periods(int64_t previous, int64_t current) {
    PeriodComparison c;
    c.current_minor = current;
    c.previous_minor = previous;
    c.delta_minor = current - previous;

    if (previous > 0) {
        c.change_ratio = static_cast<double>(c.delta_minor) / static_cast<double>(previous);
    } else if (current > 0) {
        c.from_zero = true;
        c.change_ratio = std::nullopt;   // 不从 0 算涨幅
    } else {
        c.change_ratio = 0.0;
    }

    if (c.delta_minor == 0) {
        c.direction = "flat";
    } else {
        // 「基本持平」的容忍带：±3% 以内算平，避免 1 元的变化被报成「上涨 8%」
        const double ratio = c.change_ratio.value_or(c.from_zero ? 1.0 : 0.0);
        if (previous > 0 && std::fabs(ratio) < 0.03) {
            c.direction = "flat";
        } else {
            c.direction = c.delta_minor > 0 ? "up" : "down";
        }
    }
    return c;
}

Result<TrendResult> compute_impl(const std::vector<Record>& records,
                                const std::string& category_filter,
                                const DateRange& range,
                                int window) {
    if (window < 1) window = 1;
    if (window > 31) window = 31;      // 再大就没意义了，一个月以内
    if (window % 2 == 0) window += 1;  // 居中窗口用奇数

    if (!range.from.is_valid() || !range.to.is_valid() || range.to < range.from) {
        return Result<TrendResult>::fail(ErrorCode::InvalidArgument, "趋势区间非法",
                                        "compute_expense_trend");
    }

    // 日序列（补零）
    auto dates = date_range_inclusive(range.from, range.to);
    if (dates.empty()) {
        return Result<TrendResult>::fail(ErrorCode::InvalidArgument,
                                        "区间过长或非法，无法生成趋势", "compute_expense_trend");
    }
    std::vector<int64_t> daily(dates.size(), 0);

    const int64_t base = range.from.to_epoch_days();
    for (const Record& r : records) {
        if (r.direction != Direction::Expense) continue;
        if (r.date < range.from || r.date > range.to) continue;
        if (!category_filter.empty() && r.category_id != category_filter) continue;

        const int64_t offset = r.date.to_epoch_days() - base;
        if (offset < 0 || static_cast<size_t>(offset) >= daily.size()) continue;
        daily[static_cast<size_t>(offset)] += r.amount.minor_units();
    }

    TrendResult result;
    result.range = range;
    result.window = window;
    result.points = build_points(daily, dates, window);

    result.days_with_records = 0;
    for (int64_t v : daily) {
        if (v > 0) result.days_with_records += 1;
    }

    // 环比：与紧邻的前一个等长区间比较
    {
        const int64_t span = static_cast<int64_t>(dates.size());
        const Date prev_to = range.from.add_days(-1);
        const Date prev_from = prev_to.add_days(-(span - 1));

        int64_t current_total = 0;
        for (int64_t v : daily) current_total += v;

        int64_t previous_total = 0;
        for (const Record& r : records) {
            if (r.direction != Direction::Expense) continue;
            if (r.date < prev_from || r.date > prev_to) continue;
            if (!category_filter.empty() && r.category_id != category_filter) continue;
            previous_total += r.amount.minor_units();
        }

        result.compared_to_previous = compare_periods(previous_total, current_total);
    }

    result.sufficient_data = result.days_with_records >= kMinDaysForTrend;

    if (!result.sufficient_data) {
        result.note = "本区间只有 " + std::to_string(result.days_with_records) +
                      " 天有记录，不足 " + std::to_string(kMinDaysForTrend) +
                      " 天。趋势线噪声很大，建议只看具体数字，不要把波动当规律。";
    } else {
        result.note = "趋势为 " + std::to_string(window) + " 天居中滑动平均；环比对比的是" +
                      range.from.to_string() + " 之前的等长区间。未考虑季节性因素（假期、" +
                      "开学、电商大促）。";
    }

    return Result<TrendResult>(std::move(result));
}

}  // namespace

Result<TrendResult> compute_expense_trend(const std::vector<Record>& records,
                                          const DateRange& range,
                                          int window) {
    return compute_impl(records, {}, range, window);
}

Result<TrendResult> compute_category_trend(const std::vector<Record>& records,
                                           const std::string& category_id,
                                           const DateRange& range,
                                           int window) {
    if (category_id.empty()) {
        return Result<TrendResult>::fail(ErrorCode::InvalidArgument,
                                        "分类 id 为空", "compute_category_trend");
    }
    return compute_impl(records, category_id, range, window);
}

Result<std::vector<int64_t>> daily_expense_series(const std::vector<Record>& records,
                                                  const DateRange& range) {
    auto dates = date_range_inclusive(range.from, range.to);
    if (dates.empty()) {
        return Result<std::vector<int64_t>>::fail(ErrorCode::InvalidArgument,
                                                 "区间非法或过长", "daily_expense_series");
    }

    std::vector<int64_t> daily(dates.size(), 0);
    const int64_t base = range.from.to_epoch_days();
    for (const Record& r : records) {
        if (r.direction != Direction::Expense) continue;
        if (r.date < range.from || r.date > range.to) continue;
        const int64_t offset = r.date.to_epoch_days() - base;
        if (offset < 0 || static_cast<size_t>(offset) >= daily.size()) continue;
        daily[static_cast<size_t>(offset)] += r.amount.minor_units();
    }
    return Result<std::vector<int64_t>>(std::move(daily));
}

}  // namespace penhu::analytics
