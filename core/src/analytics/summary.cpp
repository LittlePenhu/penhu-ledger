#include "penhu/analytics/summary.hpp"

#include <algorithm>
#include <unordered_map>

namespace penhu::analytics {
namespace {

/// 分类查表（一次读完，避免渲染时 N+1 查询）
using CategoryIndex = std::unordered_map<std::string, const Category*>;

CategoryIndex index_categories(const std::vector<Category>& categories) {
    CategoryIndex index;
    index.reserve(categories.size());
    for (const auto& c : categories) {
        index.emplace(c.id, &c);
    }
    return index;
}

bool in_range(const Date& d, const DateRange& r) {
    return !(d < r.from) && !(d > r.to);
}

}  // namespace

Result<std::vector<DailyPoint>> build_daily_series(const std::vector<Record>& records,
                                                   const DateRange& range) {
    if (!range.from.is_valid() || !range.to.is_valid()) {
        return Result<std::vector<DailyPoint>>::fail(ErrorCode::InvalidArgument,
                                                    "区间端点日期无效", "build_daily_series");
    }
    if (range.to < range.from) {
        return Result<std::vector<DailyPoint>>::fail(
            ErrorCode::InvalidArgument,
            "区间终点早于起点: " + range.from.to_string() + " ~ " + range.to.to_string(),
            "build_daily_series");
    }

    auto days = date_range_inclusive(range.from, range.to);
    if (days.empty()) {
        // 超长区间被 date_range_inclusive 挡下了，把原因透传出去
        return Result<std::vector<DailyPoint>>::fail(
            ErrorCode::InvalidArgument,
            "区间过长（超过 20 年），请缩小范围", "build_daily_series");
    }

    std::vector<DailyPoint> series;
    series.reserve(days.size());
    for (const Date& d : days) {
        DailyPoint p;
        p.date = d;
        series.push_back(p);
    }

    // 用 epoch_days 直接定位下标，避免每天做一次线性查找
    const int64_t base = range.from.to_epoch_days();
    for (const Record& r : records) {
        if (!in_range(r.date, range)) continue;
        const int64_t offset = r.date.to_epoch_days() - base;
        if (offset < 0 || static_cast<size_t>(offset) >= series.size()) continue;

        DailyPoint& p = series[static_cast<size_t>(offset)];
        p.record_count += 1;
        p.has_records = true;
        if (r.direction == Direction::Expense) {
            p.expense_minor += r.amount.minor_units();
        } else {
            p.income_minor += r.amount.minor_units();
        }
    }

    return Result<std::vector<DailyPoint>>(std::move(series));
}

Result<std::vector<CategoryBreakdown>> breakdown_by_category(
    const std::vector<Record>& records,
    const std::vector<Category>& categories,
    const DateRange& range,
    Direction direction) {
    const CategoryIndex index = index_categories(categories);

    struct Acc {
        int64_t total{0};
        int64_t count{0};
    };
    std::unordered_map<std::string, Acc> acc;
    int64_t grand_total = 0;

    for (const Record& r : records) {
        if (r.direction != direction) continue;
        if (!in_range(r.date, range)) continue;

        Acc& a = acc[r.category_id];
        a.total += r.amount.minor_units();
        a.count += 1;
        grand_total += r.amount.minor_units();
    }

    std::vector<CategoryBreakdown> out;
    out.reserve(acc.size());
    for (const auto& [category_id, a] : acc) {
        CategoryBreakdown b;
        b.category_id = category_id;
        b.direction = direction;
        b.total_minor = a.total;
        b.record_count = a.count;

        const auto it = index.find(category_id);
        if (it != index.end()) {
            b.category_name  = it->second->name;
            b.category_color = it->second->color_hex;
        } else {
            // 分类被删掉但记录还在（理论上不该发生，因为删除有引用检查），
            // 这里兜一下，避免界面出现空白行
            b.category_name  = "（已删除分类）";
            b.category_color = "#78909C";
        }

        b.share = grand_total > 0
                      ? static_cast<double>(a.total) / static_cast<double>(grand_total)
                      : 0.0;
        out.push_back(std::move(b));
    }

    // 金额降序；金额相同时按名字排，保证同样数据每次渲染顺序一致
    std::sort(out.begin(), out.end(), [](const CategoryBreakdown& x,
                                         const CategoryBreakdown& y) {
        if (x.total_minor != y.total_minor) return x.total_minor > y.total_minor;
        return x.category_name < y.category_name;
    });

    return Result<std::vector<CategoryBreakdown>>(std::move(out));
}

Result<Summary> summarize(const std::vector<Record>& records,
                          const std::vector<Category>& categories,
                          const DateRange& range) {
    if (!range.from.is_valid() || !range.to.is_valid() || range.to < range.from) {
        return Result<Summary>::fail(ErrorCode::InvalidArgument, "汇总区间非法", "summarize");
    }

    Summary s;
    s.range = range;

    int64_t max_single = 0;
    Date    max_single_date;

    for (const Record& r : records) {
        if (!in_range(r.date, range)) continue;

        if (r.direction == Direction::Expense) {
            s.expense_minor += r.amount.minor_units();
            s.expense_records += 1;
            if (r.amount.minor_units() > max_single) {
                max_single = r.amount.minor_units();
                max_single_date = r.date;
            }
        } else {
            s.income_minor += r.amount.minor_units();
            s.income_records += 1;
        }
    }

    s.net_minor = s.income_minor - s.expense_minor;
    s.max_expense_minor = Money::from_minor(max_single);
    s.max_expense_date = max_single_date.is_valid() ? max_single_date.to_string() : std::string{};

    // 两个不同的分母，各有各的用处，绝不能混：
    //   active_days  —— 有「任意记录」的天数，用于计算记录覆盖率
    //   支出活跃天数 —— 有「支出记录」的天数，用于计算日均支出
    // 混用会造成两种失真：把只有收入的日子算进日均分母会低估日均；
    // 而只算支出日又会让覆盖率虚高、看不出「其实漏记了很多天」。
    // 这个区分是靠单测里那条「收入那天不该拉低日均」的用例逼出来的。
    int64_t expense_active_days = 0;
    {
        std::vector<int64_t> all_days;
        std::vector<int64_t> expense_days;
        all_days.reserve(records.size());
        expense_days.reserve(records.size());

        for (const Record& r : records) {
            if (!in_range(r.date, range)) continue;
            all_days.push_back(r.date.to_epoch_days());
            if (r.direction == Direction::Expense) {
                expense_days.push_back(r.date.to_epoch_days());
            }
        }

        const auto dedupe_count = [](std::vector<int64_t>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            return static_cast<int64_t>(v.size());
        };

        s.active_days = dedupe_count(all_days);
        expense_active_days = dedupe_count(expense_days);
    }

    // 日均：分母用「有支出记录的天数」。一天都没记则给 0 而不是除零。
    // 界面上要同时展示这个口径，避免用户以为它是「整月日均」。
    if (expense_active_days > 0) {
        s.avg_daily_expense_minor = Money::from_minor(s.expense_minor / expense_active_days);
    } else {
        s.avg_daily_expense_minor = Money::from_minor(0);
    }

    auto breakdown = breakdown_by_category(records, categories, range, Direction::Expense);
    if (breakdown.is_err()) return breakdown.error();
    s.by_category = std::move(breakdown).value();

    return Result<Summary>(std::move(s));
}

Result<Dashboard> build_dashboard(const std::vector<Record>& records,
                                  const std::vector<Category>& categories,
                                  const DateRange& range) {
    Dashboard d;
    d.range = range;

    auto s = summarize(records, categories, range);
    if (s.is_err()) return s.error();
    d.summary = std::move(s).value();

    auto series = build_daily_series(records, range);
    if (series.is_err()) return series.error();
    d.daily = std::move(series).value();

    d.days_in_range = static_cast<int64_t>(d.daily.size());
    d.days_with_records = 0;
    for (const auto& p : d.daily) {
        if (p.has_records) d.days_with_records += 1;
    }

    return Result<Dashboard>(std::move(d));
}

}  // namespace penhu::analytics
