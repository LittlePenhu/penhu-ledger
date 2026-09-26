#pragma once
// =============================================================================
//  penhu/analytics/trend.hpp
//  消费趋势。
//
//  两个指标，分别回答两个不同的问题：
//    移动平均  —— 「平常大概什么水平？」去掉单日噪声，看基线
//    环比     —— 「最近比之前花得多了还是少了？」比的是两个等长区间
//
//  刻意不做「线性回归预测下个月」。理由：记账数据点太少（一个新用户
//  可能只有 20 天数据），回归出来的斜率基本是噪声，把它画成「预计下月支出」
//  是在骗用户。宁可只报事实。
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "penhu/analytics/summary.hpp"
#include "penhu/domain/category.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/util/result.hpp"

namespace penhu::analytics {

struct TrendPoint {
    Date    date;
    int64_t value_minor{0};
    /// 该点为中心的滑动平均；窗口未满时用实际可用的点数
    int64_t moving_average_minor{0};
    int     window_used{0};
};

/// 环比结果。和「上一个等长区间」比。
struct PeriodComparison {
    int64_t current_minor{0};
    int64_t previous_minor{0};
    int64_t delta_minor{0};          // current - previous
    /// 变化比例。previous == 0 时为 nullopt（不能拿 0 做分母说「涨了无穷倍」）
    std::optional<double> change_ratio;
    /// "up" / "down" / "flat"
    std::string direction{"flat"};
    /// previous == 0 但 current > 0，属于「从无到有」，单独标出来
    bool from_zero{false};
};

struct TrendResult {
    DateRange                  range;
    std::vector<TrendPoint>    points;        // 按日期升序，已补零
    int                        window{7};
    PeriodComparison           compared_to_previous;
    /// 数据够不够支撑结论。界面上要据此降级展示（不画趋势线，只列数字）
    bool                       sufficient_data{false};
    int64_t                    days_with_records{0};
    std::string                note;          // 给用户看的口径说明
};

/// 计算支出趋势。window 是滑动平均窗口（天），默认 7。
Result<TrendResult> compute_expense_trend(const std::vector<Record>& records,
                                          const DateRange& range,
                                          int window = 7);

/// 同一逻辑，但针对某个分类
Result<TrendResult> compute_category_trend(const std::vector<Record>& records,
                                           const std::string& category_id,
                                           const DateRange& range,
                                           int window = 7);

/// 补贴与补零后的日序列（趋势图直接用）
Result<std::vector<int64_t>> daily_expense_series(const std::vector<Record>& records,
                                                 const DateRange& range);

}  // namespace penhu::analytics
