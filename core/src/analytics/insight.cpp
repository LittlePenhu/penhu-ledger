#include "penhu/analytics/insight.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace penhu::analytics {
namespace {

std::string yuan(int64_t minor) {
    const bool neg = minor < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(minor) + 1ULL)
                             : static_cast<uint64_t>(minor);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s¥%llu.%02llu", neg ? "-" : "",
                  static_cast<unsigned long long>(mag / 100ULL),
                  static_cast<unsigned long long>(mag % 100ULL));
    return std::string(buf);
}

std::string percent(double ratio) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f%%", ratio * 100.0);
    return std::string(buf);
}

int severity_rank(InsightSeverity s) {
    switch (s) {
        case InsightSeverity::Warn: return 0;
        case InsightSeverity::Info: return 1;
        case InsightSeverity::Good: return 2;
    }
    return 3;
}

/// 判断「数据够不够说话」。少于 7 天有记录时任何结构性结论都不可靠。
constexpr int64_t kMinActiveDays = 7;
constexpr int64_t kMinRecords = 10;

}  // namespace

const char* to_string(InsightSeverity severity) noexcept {
    switch (severity) {
        case InsightSeverity::Good: return "good";
        case InsightSeverity::Info: return "info";
        case InsightSeverity::Warn: return "warn";
    }
    return "info";
}

InsightSet generate_insights(const Dashboard& dashboard,
                             const TrendResult& trend,
                             const AnomalyReport& anomalies) {
    InsightSet out;
    const Summary& s = dashboard.summary;

    out.data_note = "统计区间 " + dashboard.range.from.to_string() + " ~ " +
                    dashboard.range.to.to_string() + "，共 " +
                    std::to_string(dashboard.days_in_range) + " 天，其中 " +
                    std::to_string(dashboard.days_with_records) + " 天有记录，" +
                    std::to_string(s.expense_records) + " 笔支出。";

    out.actionable = dashboard.days_with_records >= kMinActiveDays &&
                     s.expense_records >= kMinRecords;

    // -------------------------------------------------------------------------
    //  规则 1：支出结构集中度
    // -------------------------------------------------------------------------
    if (!s.by_category.empty() && s.expense_minor > 0) {
        const CategoryBreakdown& top = s.by_category.front();
        if (top.share >= 0.40) {
            Insight i;
            i.rule_id = "concentration";
            i.severity = top.share >= 0.55 ? InsightSeverity::Warn : InsightSeverity::Info;
            i.title = "支出集中在「" + top.category_name + "」";
            i.category_id = top.category_id;
            i.category_name = top.category_name;
            i.related_minor = top.total_minor;
            i.detail = "「" + top.category_name + "」占本期总支出的 " + percent(top.share) +
                       "（" + yuan(top.total_minor) + "，共 " +
                       std::to_string(top.record_count) + " 笔）。" +
                       "结构单一意味着这个分类一旦涨价，你的总支出会直接被拉动。";
            out.items.push_back(std::move(i));
        }
    }

    // -------------------------------------------------------------------------
    //  规则 2：环比变化
    // -------------------------------------------------------------------------
    const PeriodComparison& cmp = trend.compared_to_previous;
    if (cmp.previous_minor > 0 && cmp.change_ratio.has_value()) {
        const double r = cmp.change_ratio.value();

        if (r >= 0.20) {
            Insight i;
            i.rule_id = "mom_up";
            i.severity = InsightSeverity::Warn;
            i.title = "支出比上一期高了 " + percent(r);
            i.related_minor = cmp.delta_minor;
            i.detail = "本期 " + yuan(cmp.current_minor) + "，上一期 " +
                       yuan(cmp.previous_minor) + "，多花了 " + yuan(cmp.delta_minor) +
                       "。注意这只说明「比之前多」，不代表花得不合理——"
                       "换季、搬家、看病都会造成这种上升。";
            out.items.push_back(std::move(i));
        } else if (r <= -0.15) {
            Insight i;
            i.rule_id = "mom_down";
            i.severity = InsightSeverity::Good;
            i.title = "支出比上一期低了 " + percent(-r);
            i.related_minor = -cmp.delta_minor;
            i.detail = "本期 " + yuan(cmp.current_minor) + "，上一期 " +
                       yuan(cmp.previous_minor) + "，少花了 " + yuan(-cmp.delta_minor) + "。";
            out.items.push_back(std::move(i));
        }
    } else if (cmp.from_zero) {
        Insight i;
        i.rule_id = "mom_from_zero";
        i.severity = InsightSeverity::Info;
        i.title = "上一期没有支出记录，无法计算环比";
        i.detail = "本期支出 " + yuan(cmp.current_minor) +
                   "，但紧邻的上一个等长区间没有任何记录。"
                   "这通常意味着那段时间漏记了，而不是真的没花钱——"
                   "环比在这种情况下的数字没有意义。";
        out.items.push_back(std::move(i));
    }

    // -------------------------------------------------------------------------
    //  规则 3：异常支出
    // -------------------------------------------------------------------------
    if (!anomalies.items.empty()) {
        int64_t total_minor = 0;
        for (const auto& a : anomalies.items) total_minor += a.amount_minor;

        Insight i;
        i.rule_id = "anomaly_detected";
        i.severity = InsightSeverity::Warn;
        i.title = "识别到 " + std::to_string(anomalies.items.size()) + " 笔异常支出";
        i.related_minor = total_minor;
        i.detail = "合计 " + yuan(total_minor) + "。判定方法：" +
                   (anomalies.items.front().method == "3sigma" ? "3σ（样本充足）"
                                                              : "IQR（样本偏少，降级方法）") +
                   "。判定口径和样本量在异常面板里有完整说明，"
                   "建议先确认这些笔是不是误记，再看是否需要调整消费。";
        out.items.push_back(std::move(i));
    } else if (anomalies.reliable) {
        Insight i;
        i.rule_id = "no_anomaly";
        i.severity = InsightSeverity::Good;
        i.title = "未发现异常支出";
        i.detail = "在 " + std::to_string(anomalies.sample_size) +
                   " 笔样本中没有金额显著偏离你自身消费习惯的记录。"
                   "注意这只说明「消费稳定」，不代表「花得少」。";
        out.items.push_back(std::move(i));
    }

    // -------------------------------------------------------------------------
    //  规则 4：记账完整性
    // -------------------------------------------------------------------------
    if (dashboard.days_in_range > 0) {
        const double coverage =
            static_cast<double>(dashboard.days_with_records) /
            static_cast<double>(dashboard.days_in_range);
        if (coverage < 0.5 && dashboard.days_in_range >= 7) {
            Insight i;
            i.rule_id = "sparse_recording";
            i.severity = InsightSeverity::Warn;
            i.title = "记录覆盖率只有 " + percent(coverage);
            i.detail = std::to_string(dashboard.days_in_range) + " 天里只有 " +
                       std::to_string(dashboard.days_with_records) + " 天有记录。" +
                       "覆盖率低会让所有统计口径失真：日均被低估、"
                       "趋势线会出现大量零值假象、异常识别会把正常消费误判为异常。"
                       "如果你只是懒得记小额支出，那所有分析都要打折看。";
            out.items.push_back(std::move(i));
        }
    }

    if (s.expense_records > 0 && s.income_records == 0) {
        Insight i;
        i.rule_id = "no_income_recorded";
        i.severity = InsightSeverity::Info;
        i.title = "本期没有收入记录";
        i.detail = "只有支出没有收入，「结余」这个指标算不出来。"
                   "如果你想知道「这个月是净赚还是净亏」，需要把收入也记上。";
        out.items.push_back(std::move(i));
    }

    // -------------------------------------------------------------------------
    //  规则 5：周末支出显著高于工作日
    // -------------------------------------------------------------------------
    {
        int64_t weekend_total = 0, weekday_total = 0;
        int64_t weekend_days = 0, weekday_days = 0;
        for (const auto& p : dashboard.daily) {
            if (p.date.is_weekend()) {
                weekend_total += p.expense_minor;
                ++weekend_days;
            } else {
                weekday_total += p.expense_minor;
                ++weekday_days;
            }
        }
        if (weekend_days > 0 && weekday_days > 0) {
            const double wd_avg = static_cast<double>(weekend_total) / weekend_days;
            const double day_avg = static_cast<double>(weekday_total) / weekday_days;
            if (day_avg > 0 && wd_avg > day_avg * 1.5) {
                Insight i;
                i.rule_id = "weekend_spike";
                i.severity = InsightSeverity::Info;
                i.title = "周末日均支出是工作日的 " + percent(wd_avg / day_avg - 1.0) + " 倍以上";
                i.related_minor = static_cast<int64_t>(wd_avg * weekend_days) - static_cast<int64_t>(day_avg * weekend_days);
                i.detail = "周末日均 " + yuan(static_cast<int64_t>(wd_avg)) + "，工作日日均 " +
                           yuan(static_cast<int64_t>(day_avg)) + "。" +
                           "如果周末的支出里有一部分是可以挪到工作日的（比如采购），"
                           "挪过去不会省钱，只是让数字好看一点——所以这条只当参考。";
                out.items.push_back(std::move(i));
            }
        }
    }

    // -------------------------------------------------------------------------
    //  规则 6：可量化的节省空间（纯算术，不带价值判断）
    // -------------------------------------------------------------------------
    if (out.actionable && s.expense_minor > 0 && !s.by_category.empty()) {
        // 取前三个分类，各算「下降 10%」的算术结果
        const size_t take = std::min<size_t>(3, s.by_category.size());
        int64_t potential = 0;
        std::string detail;
        for (size_t k = 0; k < take; ++k) {
            const CategoryBreakdown& c = s.by_category[k];
            const int64_t save = c.total_minor / 10;
            potential += save;
            detail += "  · 「" + c.category_name + "」" + yuan(c.total_minor) +
                      " → 降 10% 省 " + yuan(save) + "\n";
        }

        Insight i;
        i.rule_id = "arithmetic_saving";
        i.severity = InsightSeverity::Info;
        i.title = "算术上的节省空间：" + yuan(potential) + "（各分类降 10%）";
        i.related_minor = potential;
        i.detail = "这不是建议，是算术。它只回答「如果每个大类都少花 10%，"
                   "这一段能少支出多少」：\n" + detail +
                   "降哪一类、能不能降，取决于你的实际情况，数据里没有这个信息。";
        out.items.push_back(std::move(i));
    }

    // -------------------------------------------------------------------------
    //  排序
    // -------------------------------------------------------------------------
    std::sort(out.items.begin(), out.items.end(), [](const Insight& a, const Insight& b) {
        const int ra = severity_rank(a.severity);
        const int rb = severity_rank(b.severity);
        if (ra != rb) return ra < rb;
        if (a.related_minor != b.related_minor) return a.related_minor > b.related_minor;
        return a.rule_id < b.rule_id;
    });

    // -------------------------------------------------------------------------
    //  盲区（始终输出，界面必须能展示）
    // -------------------------------------------------------------------------
    out.limitations.push_back(
        "所有结论都基于你记录下来的数据。漏记的支出不会出现在任何统计里，"
        "而漏记往往集中在小额高频消费上——那会让「消费结构」偏向大额分类。");
    out.limitations.push_back(
        "没有你的收入水平、家庭负担、固定支出（房租/学费）和目标储蓄额，"
        "所以这里给不出「你应该花多少」这种判断。");
    out.limitations.push_back(
        "未做通胀、季节性、突发事件的校正。一次看病、一次搬家会让当期数字完全变形。");
    if (!out.actionable) {
        out.limitations.push_back(
            "当前数据量（" + std::to_string(dashboard.days_with_records) + " 天 / " +
            std::to_string(s.expense_records) + " 笔）低于生成结构性建议的门槛（" +
            std::to_string(kMinActiveDays) + " 天 / " + std::to_string(kMinRecords) +
            " 笔），上面的结论只应作为粗略参考。");
    }

    return out;
}

}  // namespace penhu::analytics
