#pragma once
// =============================================================================
//  penhu/report/prompt_builder.hpp
//  把统计结果压成 prompt。
//
//  这个模块存在的唯一理由是「本地模型只有 440M」。
//  给云端 GPT 类模型可以喂一大堆上下文，它能把噪声过滤掉；
//  440M 的模型做不到——prompt 一长，它就开始抄数字、跳段、或者干脆复读。
//  所以这里做了两档：
//
//    Compact   给本地小模型：只给必要数字，不带建议、不带局限说明、
//              不带 markdown 结构要求，句子数硬限制在 5 句以内。
//    Detailed  给云端：可以带上分类结构、环比、异常清单、数据局限，
//              并允许分小段。
//
//  两档共用同一份 ReportFacts，所以「给模型的数字」永远和「界面显示的数字」
//  是同一份——不会出现报告里写着 ¥1200、界面上显示 ¥1150 这种事。
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/insight.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"

namespace penhu::report {

enum class ReportKind {
    Daily   = 0,
    Weekly  = 1,
    Monthly = 2
};

const char* to_string(ReportKind kind) noexcept;
const char* storage_id(ReportKind kind) noexcept;      // "daily" / "weekly" / "monthly"
std::optional<ReportKind> parse_kind(std::string_view text) noexcept;
const char* label_zh(ReportKind kind) noexcept;
/// 该类型对应的日期区间
DateRange range_for(ReportKind kind, const Date& anchor);

enum class PromptStyle {
    Compact,    // 本地小模型
    Detailed    // 云端
};

const char* to_string(PromptStyle style) noexcept;
/// 根据 provider 类型挑一个合适的档位
PromptStyle style_for_provider_kind(int provider_kind) noexcept;

/// 报告所需的全部事实。**这是界面与模型共用的唯一数字来源。**
struct ReportFacts {
    ReportKind kind{ReportKind::Daily};
    DateRange  range;

    // 总体
    int64_t expense_minor{0};
    int64_t income_minor{0};
    int64_t net_minor{0};
    int64_t expense_records{0};
    int64_t income_records{0};
    int64_t days_in_range{0};
    int64_t days_with_records{0};
    int64_t avg_daily_expense_minor{0};
    int64_t max_expense_minor{0};
    std::string max_expense_date;

    // 结构（已按金额降序，调用方决定保留几个）
    // 注意命名空间：CategoryBreakdown 定义在 domain/record.hpp 的 penhu 下，
    // 而 Anomaly / Insight 定义在 analytics 下。两者限定名不一样，写错编不过。
    std::vector<CategoryBreakdown> categories;

    // 变化
    int64_t                previous_expense_minor{0};
    std::optional<double>  change_ratio;
    std::string            change_direction{"flat"};
    bool                   change_from_zero{false};

    // 异常
    std::vector<analytics::Anomaly> anomalies;
    int64_t                         anomaly_sample_size{0};
    bool                            anomaly_reliable{false};

    // 建议与局限
    std::vector<analytics::Insight> insights;
    std::vector<std::string>        limitations;
    std::string                     data_note;

    // 趋势口径说明
    std::string trend_note;
    bool        trend_sufficient{false};
};

/// 从各分析模块的输出组装事实
ReportFacts build_facts(ReportKind kind,
                        const analytics::Dashboard& dashboard,
                        const analytics::TrendResult& trend,
                        const analytics::AnomalyReport& anomalies,
                        const analytics::InsightSet& insights);

struct BuiltPrompt {
    std::string system;
    std::string user;
    PromptStyle style{PromptStyle::Compact};
    /// 粗略的字符数，用来在日志里对比「同一份数据在两档下的 prompt 大小」
    size_t system_chars{0};
    size_t user_chars{0};
};

BuiltPrompt build_prompt(const ReportFacts& facts, PromptStyle style);

/// 事实 -> JSON（存进 reports.stats_json，便于回溯「当时依据什么数字生成」）
std::string facts_to_json(const ReportFacts& facts);

}  // namespace penhu::report
