#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/summary.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace penhu::analytics {
namespace {

/// 用 3σ 判定的最小样本量。低于此值时 σ 的估计本身就不稳，
/// 3σ 区间会窄得离谱，把正常消费全打成异常。
constexpr int64_t kMinSamplesFor3Sigma = 8;
/// IQR 至少要有 4 个点才有意义（否则 Q1/Q3 就是极值本身）
constexpr int64_t kMinSamplesForIqr = 4;

struct Stats {
    int64_t n{0};
    double  mean{0.0};
    double  sd{0.0};      // 样本标准差（n-1）
    double  q1{0.0};
    double  q3{0.0};
    double  iqr{0.0};
    bool    valid{false};
};

double quantile(const std::vector<int64_t>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return static_cast<double>(sorted[0]);

    const double pos = q * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);

    const double a = static_cast<double>(sorted[lo]);
    const double b = static_cast<double>(sorted[hi]);
    return a * (1.0 - frac) + b * frac;
}

Stats compute_stats(std::vector<int64_t> values) {
    Stats s;
    s.n = static_cast<int64_t>(values.size());
    if (values.empty()) return s;

    std::sort(values.begin(), values.end());

    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    s.mean = sum / static_cast<double>(values.size());

    if (values.size() >= 2) {
        double sq = 0.0;
        for (int64_t v : values) {
            const double d = static_cast<double>(v) - s.mean;
            sq += d * d;
        }
        s.sd = std::sqrt(sq / static_cast<double>(values.size() - 1));
    }

    s.q1 = quantile(values, 0.25);
    s.q3 = quantile(values, 0.75);
    s.iqr = s.q3 - s.q1;
    s.valid = true;
    return s;
}

/// 一个「阈值 + 方法」的判定结果
struct Threshold {
    double      value{0.0};
    std::string method;
    bool        usable{false};
    std::string description;
};

Threshold pick_threshold(const Stats& s, const std::string& scope_label) {
    Threshold t;

    // 所有值完全相同：标准差为 0，3σ 区间会退化成一个点，
    // 于是「比这个点高一分钱」都会被标成异常。这不是异常识别，是噪声。
    // 这种情况在真实数据里会出现（比如有人每天固定记同一笔通勤费）。
    if (s.n >= kMinSamplesFor3Sigma && s.sd <= 0.0) {
        t.method = "uniform";
        t.usable = false;
        t.description = scope_label + "：全部 " + std::to_string(s.n) +
                        " 笔金额完全相同，没有分布可言，不做异常判定";
        return t;
    }

    // 3σ 需要样本量 + 非零标准差
    if (s.n >= kMinSamplesFor3Sigma && s.sd > 0.0) {
        t.value = s.mean + 3.0 * s.sd;
        t.method = "3sigma";
        t.usable = true;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "%s：基于 %lld 笔样本的 3σ 阈值 = 均值 ¥%.2f + 3×标准差 ¥%.2f = ¥%.2f",
                      scope_label.c_str(), static_cast<long long>(s.n), s.mean / 100.0,
                      s.sd / 100.0, t.value / 100.0);
        t.description = buf;
        return t;
    }

    if (s.n >= kMinSamplesForIqr && s.iqr > 0.0) {
        t.value = s.q3 + 1.5 * s.iqr;
        t.method = "iqr";
        t.usable = true;
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "%s：样本仅 %lld 笔（不足 %lld），降级用 IQR = Q3 ¥%.2f + 1.5×IQR ¥%.2f = ¥%.2f",
                      scope_label.c_str(), static_cast<long long>(s.n),
                      static_cast<long long>(kMinSamplesFor3Sigma), s.q3 / 100.0,
                      s.iqr / 100.0, t.value / 100.0);
        t.description = buf;
        return t;
    }

    t.method = "insufficient";
    t.usable = false;
    t.description = scope_label + "：样本仅 " + std::to_string(s.n) +
                    " 笔（IQR 也需要 " + std::to_string(kMinSamplesForIqr) +
                    " 笔），不做判定";
    return t;
}

std::string format_yuan(int64_t minor) {
    const bool neg = minor < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(minor) + 1ULL)
                             : static_cast<uint64_t>(minor);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s¥%llu.%02llu", neg ? "-" : "",
                  static_cast<unsigned long long>(mag / 100ULL),
                  static_cast<unsigned long long>(mag % 100ULL));
    return std::string(buf);
}

}  // namespace

const char* to_string(AnomalyKind kind) noexcept {
    switch (kind) {
        case AnomalyKind::SingleExpense:      return "single_expense";
        case AnomalyKind::DailyTotal:         return "daily_total";
        case AnomalyKind::CategoryDailyTotal: return "category_daily_total";
    }
    return "unknown";
}

Result<AnomalyReport> detect_anomalies(const std::vector<Record>& records,
                                       const std::vector<Category>& categories,
                                       const DateRange& range,
                                       int max_items) {
    if (max_items < 1) max_items = 1;

    // 分类查表
    std::unordered_map<std::string, const Category*> cat_index;
    for (const auto& c : categories) cat_index.emplace(c.id, &c);

    // 收区间内的支出
    std::vector<const Record*> expenses;
    for (const Record& r : records) {
        if (r.direction != Direction::Expense) continue;
        if (r.date < range.from || r.date > range.to) continue;
        expenses.push_back(&r);
    }

    AnomalyReport report;
    report.sample_size = static_cast<int64_t>(expenses.size());

    if (expenses.empty()) {
        report.method_summary = "区间内没有支出记录，无需判定。";
        report.limitations.push_back("没有数据可分析。");
        return Result<AnomalyReport>(std::move(report));
    }

    // ---------------------------------------------------------------------
    //  按分类分组
    // ---------------------------------------------------------------------
    std::unordered_map<std::string, std::vector<const Record*>> by_category;
    for (const Record* r : expenses) by_category[r->category_id].push_back(r);

    std::vector<std::string> method_notes;
    std::vector<Anomaly> hits;

    // 去重加入说明。同一条口径说明不该在报告里出现十遍。
    const auto add_note = [&method_notes](const std::string& note) {
        if (note.empty()) return;
        if (std::find(method_notes.begin(), method_notes.end(), note) == method_notes.end()) {
            method_notes.push_back(note);
        }
    };

    // ---------------------------------------------------------------------
    //  第一轮判定
    // ---------------------------------------------------------------------
    auto scan_single_expenses = [&](std::vector<Anomaly>& out) {
        // 全局基准（分类样本不足时的退路）
        std::vector<int64_t> all_amounts;
        all_amounts.reserve(expenses.size());
        for (const Record* r : expenses) all_amounts.push_back(r->amount.minor_units());
        const Stats global_stats = compute_stats(all_amounts);
        const Threshold global_t = pick_threshold(global_stats, "全部支出");

        // 关键：无论这条阈值「可用」还是「不可用」，都要记进说明。
        // 不可用也是一个结论（比如「所有金额完全相同，没有分布可言」），
        // 把它丢掉会让报告退化成「样本量不足」——那是错误归因。
        add_note(global_t.description);

        for (const auto& [category_id, list] : by_category) {
            std::vector<int64_t> amounts;
            amounts.reserve(list.size());
            for (const Record* r : list) amounts.push_back(r->amount.minor_units());

            const Stats cat_stats = compute_stats(amounts);
            const std::string cat_name =
                cat_index.count(category_id) ? cat_index[category_id]->name : "未知分类";
            const Threshold cat_t = pick_threshold(cat_stats, "分类「" + cat_name + "」");

            // 分类自己的阈值可用时才把它写进口径说明，
            // 否则一个用户有十个分类就会刷出十条「样本不足」，没意义。
            if (cat_t.usable) add_note(cat_t.description);

            const Threshold& t = cat_t.usable ? cat_t : global_t;
            if (!t.usable) continue;

            for (const Record* r : list) {
                const double amount = static_cast<double>(r->amount.minor_units());
                if (amount <= t.value) continue;

                Anomaly a;
                a.kind = AnomalyKind::SingleExpense;
                a.date = r->date;
                a.category_id = category_id;
                a.category_name = cat_name;
                a.record_id = r->id;
                a.amount_minor = r->amount.minor_units();
                a.threshold_minor = t.value;
                a.score = t.value > 0.0 ? amount / t.value : 0.0;
                a.method = t.method;

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "%s 在「%s」花了 %s，超过判定阈值 %s 的 %.1f 倍",
                              r->date.to_string().c_str(), cat_name.c_str(),
                              format_yuan(r->amount.minor_units()).c_str(),
                              format_yuan(static_cast<int64_t>(t.value)).c_str(),
                              a.score);
                a.explanation = buf;
                out.push_back(std::move(a));
            }
        }
    };

    scan_single_expenses(hits);

    // ---------------------------------------------------------------------
    //  第二轮：剔除首轮命中项后重算（防止异常值把阈值撑高导致漏报）
    // ---------------------------------------------------------------------
    if (!hits.empty()) {
        std::vector<std::string> flagged_ids;
        flagged_ids.reserve(hits.size());
        for (const auto& a : hits) flagged_ids.push_back(a.record_id);

        std::vector<int64_t> residuals;
        residuals.reserve(expenses.size());
        for (const Record* r : expenses) {
            const bool flagged =
                std::find(flagged_ids.begin(), flagged_ids.end(), r->id) != flagged_ids.end();
            if (!flagged) residuals.push_back(r->amount.minor_units());
        }

        const Stats residual_stats = compute_stats(residuals);
        const Threshold residual_t = pick_threshold(residual_stats, "剔除首轮异常后");

        if (residual_t.usable) {
            method_notes.push_back(residual_t.description + "（第二轮）");

            std::vector<Anomaly> second_pass;
            for (const Record* r : expenses) {
                const bool already =
                    std::find(flagged_ids.begin(), flagged_ids.end(), r->id) != flagged_ids.end();
                if (already) continue;

                const double amount = static_cast<double>(r->amount.minor_units());
                if (amount <= residual_t.value) continue;

                const std::string cat_name = cat_index.count(r->category_id)
                                                 ? cat_index[r->category_id]->name
                                                 : "未知分类";
                Anomaly a;
                a.kind = AnomalyKind::SingleExpense;
                a.date = r->date;
                a.category_id = r->category_id;
                a.category_name = cat_name;
                a.record_id = r->id;
                a.amount_minor = r->amount.minor_units();
                a.threshold_minor = residual_t.value;
                a.score = residual_t.value > 0.0 ? amount / residual_t.value : 0.0;
                a.method = residual_t.method;

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "%s 在「%s」花了 %s，剔除其他异常后仍超过阈值 %s 的 %.1f 倍",
                              r->date.to_string().c_str(), cat_name.c_str(),
                              format_yuan(r->amount.minor_units()).c_str(),
                              format_yuan(static_cast<int64_t>(residual_t.value)).c_str(),
                              a.score);
                a.explanation = buf;
                second_pass.push_back(std::move(a));
            }
            for (auto& a : second_pass) hits.push_back(std::move(a));
        }
    }

    // ---------------------------------------------------------------------
    //  单日总额异常
    // ---------------------------------------------------------------------
    {
        auto series = build_daily_series(records, range);
        if (series.is_ok()) {
            std::vector<int64_t> day_totals;
            for (const auto& p : series.value()) {
                if (p.has_records) day_totals.push_back(p.expense_minor);
            }
            report.scanned_days = static_cast<int64_t>(day_totals.size());

            const Stats day_stats = compute_stats(day_totals);
            const Threshold day_t = pick_threshold(day_stats, "单日支出总额");

            // 同样：可用与否都记进口径说明
            add_note(day_t.description);

            if (day_t.usable) {
                for (const auto& p : series.value()) {
                    if (!p.has_records) continue;
                    const double total = static_cast<double>(p.expense_minor);
                    if (total <= day_t.value) continue;

                    Anomaly a;
                    a.kind = AnomalyKind::DailyTotal;
                    a.date = p.date;
                    a.amount_minor = p.expense_minor;
                    a.threshold_minor = day_t.value;
                    a.score = day_t.value > 0.0 ? total / day_t.value : 0.0;
                    a.method = day_t.method;

                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "%s 全天支出 %s（%lld 笔），是阈值 %s 的 %.1f 倍",
                                  p.date.to_string().c_str(),
                                  format_yuan(p.expense_minor).c_str(),
                                  static_cast<long long>(p.record_count),
                                  format_yuan(static_cast<int64_t>(day_t.value)).c_str(),
                                  a.score);
                    a.explanation = buf;
                    hits.push_back(std::move(a));
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    //  排序、截断、口径说明
    // ---------------------------------------------------------------------
    std::sort(hits.begin(), hits.end(), [](const Anomaly& x, const Anomaly& y) {
        if (x.amount_minor != y.amount_minor) return x.amount_minor > y.amount_minor;
        return x.date > y.date;
    });

    if (static_cast<int>(hits.size()) > max_items) {
        report.suppressed_count = static_cast<int64_t>(hits.size()) - max_items;
        hits.resize(static_cast<size_t>(max_items));
    }
    report.items = std::move(hits);

    report.reliable = report.sample_size >= kMinSamplesFor3Sigma;

    {
        std::string summary = "共扫描 " + std::to_string(report.sample_size) + " 笔支出、";
        summary += std::to_string(report.scanned_days) + " 个有记录的日期。判定口径：\n";
        for (const auto& note : method_notes) {
            summary += "  · " + note + "\n";
        }
        if (method_notes.empty()) {
            summary += "  · 样本量不足，未做任何统计判定。\n";
        }
        report.method_summary = std::move(summary);
    }

    // 已知失败模式，明写出来。用户有权知道这套判定什么时候会错。
    report.limitations.push_back(
        "只识别「金额偏大」，识别不了「这笔本身就不该花」——那需要知道你的目标，"
        "光看数字判断不出来。");
    report.limitations.push_back(
        "阈值由你自己的历史数据算出。如果你从来只记大额支出，那大额就是你的常态，"
        "不会被标出来；反之刚记账的前几周容易误报。");
    if (!report.reliable) {
        report.limitations.push_back(
            "当前样本量偏小（" + std::to_string(report.sample_size) +
            " 笔），统计判定不稳，建议至少积累一个月数据后再看异常。");
    }
    report.limitations.push_back(
        "未考虑季节性。换季买衣服、过年发红包、开学买书都会被判成异常，"
        "但它们是可预期的。");
    report.limitations.push_back(
        "一次性大额（比如买电脑）和持续性支出上涨（比如房租涨了）会被混为一谈。");

    return Result<AnomalyReport>(std::move(report));
}

}  // namespace penhu::analytics
