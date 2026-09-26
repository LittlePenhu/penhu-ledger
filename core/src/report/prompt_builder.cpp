#include "penhu/report/prompt_builder.hpp"
#include "penhu/storage/report_repository.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace penhu::report {
namespace {

using json = nlohmann::json;

std::string yuan(int64_t minor) {
    const bool neg = minor < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(minor) + 1ULL)
                             : static_cast<uint64_t>(minor);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s%.2f", neg ? "-" : "",
                  static_cast<double>(mag) / 100.0);
    return std::string(buf);
}

std::string percent(double ratio) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f%%", ratio * 100.0);
    return std::string(buf);
}

const char* kind_zh(ReportKind kind) {
    switch (kind) {
        case ReportKind::Daily:   return "当日";
        case ReportKind::Weekly:  return "本周";
        case ReportKind::Monthly: return "本月";
    }
    return "当期";
}

// -----------------------------------------------------------------------------
//  Compact 档：给 440M 这个量级的本地模型
// -----------------------------------------------------------------------------
const char* kCompactSystem =
    "你是一个记账助手。用户会给你一段消费统计。\n"
    "请写一段中文分析，规则：\n"
    "1. 只能使用给出的数字，一个数字都不能编。\n"
    "2. 不要评价用户该不该花钱，只描述事实。\n"
    "3. 写 3 到 5 句话。不要标题，不要列表，不要星号。\n"
    "4. 数字太少就直接说数据不足。\n";

// -----------------------------------------------------------------------------
//  Detailed 档：给云端
// -----------------------------------------------------------------------------
const char* kDetailedSystem =
    "你是一个帮用户复盘个人支出的记账助手。\n"
    "你会收到一份结构化的支出统计（金额单位为元）。\n"
    "\n"
    "写作要求：\n"
    "1. 只使用给定数字。禁止推算、禁止举例、禁止补全你没看到的数据。\n"
    "2. 不做价值判断。不说「应该节省」；可以说「若该分类下降 10%，对应金额为 X」。\n"
    "3. 结构：先一句总体情况，再说支出结构，再说与上期的变化，然后点出需要留意的记录，"
    "最后一句说明这份分析的局限。共 4 到 6 段，每段 2 到 3 句。\n"
    "4. 用中文。不要 Markdown 标题，不要表格，不要星号。\n"
    "5. 如果某项数据缺失或样本不足，直接说明「该项数据不足」，不要绕过去。\n";

std::string compact_user_prompt(const ReportFacts& f) {
    std::ostringstream os;
    os << kind_zh(f.kind) << "统计（" << f.range.from.to_string() << " 至 "
       << f.range.to.to_string() << "）：\n";
    os << "总支出 " << yuan(f.expense_minor) << " 元\n";
    os << "总笔数 " << f.expense_records << "\n";
    if (f.income_minor > 0) {
        os << "总收入 " << yuan(f.income_minor) << " 元，结余 " << yuan(f.net_minor) << " 元\n";
    }
    os << "有记录的天数 " << f.days_with_records << " / " << f.days_in_range << "\n";

    // 只给前 3 个分类，再多小模型就抓不住重点了
    const size_t take = std::min<size_t>(3, f.categories.size());
    if (take > 0) {
        os << "支出最多的分类：\n";
        for (size_t i = 0; i < take; ++i) {
            const auto& c = f.categories[i];
            os << "  " << c.category_name << " " << yuan(c.total_minor) << " 元 占 "
               << percent(c.share) << "\n";
        }
    }

    if (f.change_ratio.has_value()) {
        os << "比上一期" << (f.change_direction == "up" ? "多" : (f.change_direction == "down" ? "少" : "基本持平"))
           << " " << percent(std::abs(f.change_ratio.value())) << "\n";
    } else if (f.change_from_zero) {
        os << "上一期没有记录，无法比较\n";
    }

    if (!f.anomalies.empty()) {
        os << "有 " << f.anomalies.size() << " 笔金额明显偏高的支出，其中最大一笔 "
           << yuan(f.anomalies.front().amount_minor) << " 元\n";
    }

    os << "请写分析。";
    return os.str();
}

std::string detailed_user_prompt(const ReportFacts& f) {
    std::ostringstream os;
    os << "## " << kind_zh(f.kind) << "支出统计\n";
    os << "统计区间：" << f.range.from.to_string() << " 至 " << f.range.to.to_string()
       << "（共 " << f.days_in_range << " 天）\n";
    os << "口径说明：" << f.data_note << "\n\n";

    os << "## 总体\n";
    os << "- 总支出：" << yuan(f.expense_minor) << " 元，共 " << f.expense_records << " 笔\n";
    os << "- 总收入：" << yuan(f.income_minor) << " 元，共 " << f.income_records << " 笔\n";
    os << "- 结余：" << yuan(f.net_minor) << " 元\n";
    os << "- 日均支出（分母为有记录的天数 " << f.days_with_records << " 天）："
       << yuan(f.avg_daily_expense_minor) << " 元\n";
    if (f.max_expense_minor > 0) {
        os << "- 单笔最大支出：" << yuan(f.max_expense_minor) << " 元，发生在 "
           << f.max_expense_date << "\n";
    }
    os << "\n";

    if (!f.categories.empty()) {
        os << "## 支出结构\n";
        const size_t take = std::min<size_t>(8, f.categories.size());
        for (size_t i = 0; i < take; ++i) {
            const auto& c = f.categories[i];
            os << "- " << c.category_name << "：" << yuan(c.total_minor) << " 元（"
               << c.record_count << " 笔，占 " << percent(c.share) << "）\n";
        }
        if (f.categories.size() > take) {
            os << "- 其余 " << (f.categories.size() - take) << " 个分类合计："
               << yuan(0) << " 元（未逐一列出）\n";
        }
        os << "\n";
    }

    os << "## 与上一期比较\n";
    if (f.change_ratio.has_value()) {
        os << "- 上一期支出 " << yuan(f.previous_expense_minor) << " 元，本期 "
           << yuan(f.expense_minor) << " 元，变化 "
           << (f.change_ratio.value() >= 0 ? "上升 " : "下降 ")
           << percent(std::abs(f.change_ratio.value())) << "\n";
    } else if (f.change_from_zero) {
        os << "- 上一期没有任何记录，无法计算变化（这通常意味着漏记，而不是真的没支出）\n";
    } else {
        os << "- 两期均为零，无变化\n";
    }
    os << "\n";

    if (!f.anomalies.empty()) {
        os << "## 需要留意的记录\n";
        const size_t take = std::min<size_t>(5, f.anomalies.size());
        for (size_t i = 0; i < take; ++i) {
            const auto& a = f.anomalies[i];
            os << "- " << a.explanation << "\n";
        }
        os << "判定基于 " << f.anomaly_sample_size << " 笔样本，方法为 "
           << (f.anomalies.front().method == "3sigma" ? "3σ" : "IQR（样本偏少时的降级方法）")
           << "。\n\n";
    } else if (f.anomaly_reliable) {
        os << "## 需要留意的记录\n- 在 " << f.anomaly_sample_size
           << " 笔样本中未检测到金额异常。\n\n";
    }

    if (!f.insights.empty()) {
        os << "## 规则引擎已经算出的要点\n";
        for (const auto& i : f.insights) {
            os << "- [" << analytics::to_string(i.severity) << "] " << i.title << "：" 
               << i.detail << "\n";
        }
        os << "\n";
    }

    if (!f.limitations.empty()) {
        os << "## 这份分析的已知局限\n";
        for (const auto& l : f.limitations) {
            os << "- " << l << "\n";
        }
        os << "\n";
    }

    os << "请按系统提示的结构写出分析。";
    return os.str();
}

}  // namespace

// -----------------------------------------------------------------------------
//  枚举
// -----------------------------------------------------------------------------

const char* to_string(ReportKind kind) noexcept {
    switch (kind) {
        case ReportKind::Daily:   return "Daily";
        case ReportKind::Weekly:  return "Weekly";
        case ReportKind::Monthly: return "Monthly";
    }
    return "Unknown";
}

const char* storage_id(ReportKind kind) noexcept {
    switch (kind) {
        case ReportKind::Daily:   return storage::report_kind::kDaily;
        case ReportKind::Weekly:  return storage::report_kind::kWeekly;
        case ReportKind::Monthly: return storage::report_kind::kMonthly;
    }
    return "daily";
}

std::optional<ReportKind> parse_kind(std::string_view text) noexcept {
    if (text == "daily" || text == "日" || text == "每日") return ReportKind::Daily;
    if (text == "weekly" || text == "周" || text == "每周") return ReportKind::Weekly;
    if (text == "monthly" || text == "月" || text == "每月") return ReportKind::Monthly;
    return std::nullopt;
}

const char* label_zh(ReportKind kind) noexcept {
    switch (kind) {
        case ReportKind::Daily:   return "每日";
        case ReportKind::Weekly:  return "每周";
        case ReportKind::Monthly: return "每月";
    }
    return "报告";
}

DateRange range_for(ReportKind kind, const Date& anchor) {
    switch (kind) {
        case ReportKind::Daily:   return DateRange{anchor, anchor};
        case ReportKind::Weekly:  return week_range(anchor);
        case ReportKind::Monthly: return month_range(anchor);
    }
    return DateRange{anchor, anchor};
}

const char* to_string(PromptStyle style) noexcept {
    switch (style) {
        case PromptStyle::Compact:  return "Compact";
        case PromptStyle::Detailed: return "Detailed";
    }
    return "Compact";
}

PromptStyle style_for_provider_kind(int provider_kind) noexcept {
    // 1 = LocalLlama, 2 = CloudOpenAi（与 llm::ProviderKind 对应，
    // 这里用 int 是为了不让 report 层依赖 llm 层）
    return provider_kind == 2 ? PromptStyle::Detailed : PromptStyle::Compact;
}

// -----------------------------------------------------------------------------
//  组装
// -----------------------------------------------------------------------------

ReportFacts build_facts(ReportKind kind,
                        const analytics::Dashboard& dashboard,
                        const analytics::TrendResult& trend,
                        const analytics::AnomalyReport& anomalies,
                        const analytics::InsightSet& insights) {
    ReportFacts f;
    f.kind = kind;
    f.range = dashboard.range;

    const auto& s = dashboard.summary;
    f.expense_minor = s.expense_minor;
    f.income_minor = s.income_minor;
    f.net_minor = s.net_minor;
    f.expense_records = s.expense_records;
    f.income_records = s.income_records;
    f.days_in_range = dashboard.days_in_range;
    f.days_with_records = dashboard.days_with_records;
    f.avg_daily_expense_minor = s.avg_daily_expense_minor.minor_units();
    f.max_expense_minor = s.max_expense_minor.minor_units();
    f.max_expense_date = s.max_expense_date;
    f.categories = s.by_category;

    f.previous_expense_minor = trend.compared_to_previous.previous_minor;
    f.change_ratio = trend.compared_to_previous.change_ratio;
    f.change_direction = trend.compared_to_previous.direction;
    f.change_from_zero = trend.compared_to_previous.from_zero;

    f.anomalies = anomalies.items;
    f.anomaly_sample_size = anomalies.sample_size;
    f.anomaly_reliable = anomalies.reliable;

    f.insights = insights.items;
    f.limitations = insights.limitations;
    // 异常模块的局限同样重要，合并进去——模型和用户都该看到
    for (const auto& l : anomalies.limitations) {
        f.limitations.push_back(l);
    }
    f.data_note = insights.data_note;
    f.trend_note = trend.note;
    f.trend_sufficient = trend.sufficient_data;

    return f;
}

BuiltPrompt build_prompt(const ReportFacts& facts, PromptStyle style) {
    BuiltPrompt out;
    out.style = style;

    if (style == PromptStyle::Compact) {
        out.system = kCompactSystem;
        out.user = compact_user_prompt(facts);
    } else {
        out.system = kDetailedSystem;
        out.user = detailed_user_prompt(facts);
    }

    out.system_chars = out.system.size();
    out.user_chars = out.user.size();
    return out;
}

std::string facts_to_json(const ReportFacts& facts) {
    json j;
    j["kind"] = storage_id(facts.kind);
    j["range"] = {{"from", facts.range.from.to_string()}, {"to", facts.range.to.to_string()}};
    j["expense_minor"] = facts.expense_minor;
    j["income_minor"] = facts.income_minor;
    j["net_minor"] = facts.net_minor;
    j["expense_records"] = facts.expense_records;
    j["income_records"] = facts.income_records;
    j["days_in_range"] = facts.days_in_range;
    j["days_with_records"] = facts.days_with_records;
    j["avg_daily_expense_minor"] = facts.avg_daily_expense_minor;
    j["max_expense_minor"] = facts.max_expense_minor;
    j["max_expense_date"] = facts.max_expense_date;

    j["categories"] = json::array();
    for (const auto& c : facts.categories) {
        j["categories"].push_back({
            {"id", c.category_id},
            {"name", c.category_name},
            {"total_minor", c.total_minor},
            {"record_count", c.record_count},
            {"share", c.share},
        });
    }

    j["previous_expense_minor"] = facts.previous_expense_minor;
    j["change_direction"] = facts.change_direction;
    j["change_from_zero"] = facts.change_from_zero;
    if (facts.change_ratio.has_value()) {
        j["change_ratio"] = facts.change_ratio.value();
    } else {
        j["change_ratio"] = nullptr;
    }

    j["anomalies"] = json::array();
    for (const auto& a : facts.anomalies) {
        j["anomalies"].push_back({
            {"kind", analytics::to_string(a.kind)},
            {"date", a.date.to_string()},
            {"category_name", a.category_name},
            {"amount_minor", a.amount_minor},
            {"threshold_minor", a.threshold_minor},
            {"score", a.score},
            {"method", a.method},
        });
    }
    j["anomaly_sample_size"] = facts.anomaly_sample_size;
    j["anomaly_reliable"] = facts.anomaly_reliable;

    j["insights"] = json::array();
    for (const auto& i : facts.insights) {
        j["insights"].push_back({
            {"rule_id", i.rule_id},
            {"severity", analytics::to_string(i.severity)},
            {"title", i.title},
            {"related_minor", i.related_minor},
        });
    }
    j["limitations"] = facts.limitations;
    j["data_note"] = facts.data_note;
    j["trend_note"] = facts.trend_note;
    j["trend_sufficient"] = facts.trend_sufficient;

    return j.dump(2);
}

}  // namespace penhu::report
