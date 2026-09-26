#pragma once
// =============================================================================
//  penhu/analytics/insight.hpp
//  规则版建议引擎。
//
//  立论前提（写在最前面，因为它决定了这个模块不做什么）：
//    我只有你的记账数字，没有你的收入目标、固定支出结构、人生计划。
//    所以「你应该少点外卖」这种话我说不出来——那不是从数据里推出来的，
//    是从我的生活经验里编出来的。
//    能说的只有算术：「餐饮占总支出 46%，若下降 10% 则本期少支出 ¥xx」。
//
//  这不是谦辞，是接口设计：
//    每条 Insight 都带 related_minor 这个具体数字，没有数字的话就不该产生这条建议。
//    全部 rules 都写成 id + 阈值 的形式，便于前端做「不再提示」以及以后调参。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"

namespace penhu::analytics {

enum class InsightSeverity {
    Good,   // 好现象，值得说一句
    Info,   // 中性提醒
    Warn    // 值得关注
};

const char* to_string(InsightSeverity severity) noexcept;

struct Insight {
    std::string     rule_id;          // 稳定 id，前端据此做「不再提示」
    InsightSeverity severity{InsightSeverity::Info};
    std::string     title;
    std::string     detail;
    int64_t         related_minor{0}; // 这条建议对应的具体金额
    std::string     category_id;
    std::string     category_name;
};

struct InsightSet {
    std::vector<Insight>     items;       // 按严重度降序，同级按金额降序
    std::vector<std::string> limitations; // 这套规则的已知盲区，界面必须能展示
    bool                     actionable{false}; // 数据够不够支撑建议
    std::string              data_note;         // 口径说明（用了多少天/多少笔）
};

/// 生成建议。三个输入分别负责不同侧面：汇总看结构、趋势看变化、异常看单点。
InsightSet generate_insights(const Dashboard& dashboard,
                             const TrendResult& trend,
                             const AnomalyReport& anomalies);

}  // namespace penhu::analytics
