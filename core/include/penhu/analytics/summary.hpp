#pragma once
// =============================================================================
//  penhu/analytics/summary.hpp
//  区间汇总 + 按日序列。
//
//  一条贯穿整个 analytics 层的原则：
//    所有金额计算都在 int64「分」上做，只在最后成比例的时候才转 double。
//    先转 double 再累加会让日汇总和月汇总对不上，而用户瞟一眼就会发现
//    「这三天加起来不等于周汇总」——这种不一致会让整个应用失去信任。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "penhu/domain/category.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/util/result.hpp"

namespace penhu::analytics {

/// 每日一个数据点。没有记账的日子也要占一格（值为 0），
/// 否则趋势图的横轴会把「没花钱的周末」压缩掉，看起来像是天天在花。
struct DailyPoint {
    Date    date;
    int64_t expense_minor{0};
    int64_t income_minor{0};
    int64_t record_count{0};
    bool    has_records{false};
};

/// 一个区间的完整视图，界面首屏就用它
struct Dashboard {
    DateRange               range;
    Summary                 summary;
    std::vector<DailyPoint> daily;        // 按日期升序，长度 = 区间天数
    int64_t                 days_in_range{0};
    int64_t                 days_with_records{0};
};

/// 按区间汇总。records 里超出区间的记录会被忽略（调用方可以放心传全量）。
Result<Summary> summarize(const std::vector<Record>& records,
                          const std::vector<Category>& categories,
                          const DateRange& range);

/// 构造首屏视图（汇总 + 补零的日序列）
Result<Dashboard> build_dashboard(const std::vector<Record>& records,
                                  const std::vector<Category>& categories,
                                  const DateRange& range);

/// 只算每日序列，不做分类占比（趋势模块用）
Result<std::vector<DailyPoint>> build_daily_series(const std::vector<Record>& records,
                                                   const DateRange& range);

/// 分类占比。share 之和在浮点意义上可能不是精确的 1.0，
/// 界面渲染百分比时应当用四舍五入到整数并接受 99%/101% 的偏差，
/// 不要为了凑 100% 去改某个分类的数值。
Result<std::vector<CategoryBreakdown>> breakdown_by_category(
    const std::vector<Record>& records,
    const std::vector<Category>& categories,
    const DateRange& range,
    Direction direction);

}  // namespace penhu::analytics
