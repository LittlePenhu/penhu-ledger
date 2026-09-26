// =============================================================================
//  tests/test_analytics.cpp
//  统计层单测：汇总、趋势、异常识别、建议规则、prompt 构造。
//
//  这一套是「纯逻辑」测试，零三方依赖，几秒能跑完。
//  重点覆盖那些「错了也不会崩、但数字会骗人」的地方——
//  比如分母用错（区间天数 vs 有记录天数）、滑动平均窗口偏了、
//  样本不足时还硬给结论。
// =============================================================================

#include "tiny_test.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/insight.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"
#include "penhu/report/prompt_builder.hpp"

using namespace penhu;
using namespace penhu::analytics;

namespace {

/// 造一批测试分类
std::vector<Category> make_categories() {
    std::vector<Category> cats;
    const char* names[] = {"餐饮", "交通", "购物", "工资"};
    const char* colors[] = {"#FF7043", "#42A5F5", "#AB47BC", "#66BB6A"};
    const Direction dirs[] = {Direction::Expense, Direction::Expense,
                              Direction::Expense, Direction::Income};
    for (int i = 0; i < 4; ++i) {
        Category c;
        c.id = std::string("cat") + std::to_string(i);
        c.user_id = "user1";
        c.name = names[i];
        c.icon = "label";
        c.color_hex = colors[i];
        c.direction = dirs[i];
        c.is_builtin = true;
        cats.push_back(c);
    }
    return cats;
}

Record make_record(const std::string& id,
                   int64_t amount_minor,
                   const std::string& category_id,
                   int year, int month, int day,
                   Direction dir = Direction::Expense,
                   const std::string& note = {}) {
    Record r;
    r.id = id;
    r.user_id = "user1";
    r.amount = Money::from_minor(amount_minor);
    r.direction = dir;
    r.category_id = category_id;
    r.date = Date{year, static_cast<unsigned>(month), static_cast<unsigned>(day)};
    r.note = note;
    r.created_at = 1000;
    r.updated_at = 1000;
    return r;
}

const DateRange kSept = DateRange{Date{2026, 9, 1}, Date{2026, 9, 30}};

}  // namespace

// -----------------------------------------------------------------------------
//  汇总
// -----------------------------------------------------------------------------

TT_TEST(Summary, 基本汇总) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("r1", 1000, "cat0", 2026, 9, 1),
        make_record("r2", 2000, "cat0", 2026, 9, 1),
        make_record("r3", 500, "cat1", 2026, 9, 2),
        make_record("r4", 100000, "cat3", 2026, 9, 5, Direction::Income),
    };

    TT_TRY_ASSIGN(s, summarize(records, cats, kSept));
    TT_EQ(s.expense_minor, int64_t{3500});
    TT_EQ(s.income_minor, int64_t{100000});
    TT_EQ(s.net_minor, int64_t{96500});
    TT_EQ(s.expense_records, int64_t{3});
    TT_EQ(s.income_records, int64_t{1});
    // active_days 统计的是「有任意记录的天数」：9/1、9/2（支出）+ 9/5（收入）= 3
    TT_EQ(s.active_days, int64_t{3});
    // 但日均支出必须按「有支出记录的天数」算：3500 / 2 天 = 1750，
    // 而不是 3500 / 3 = 1166。收入那天不该拉低日均——这是两个不同的分母。
    TT_EQ(s.avg_daily_expense_minor.minor_units(), int64_t{1750});
    // 单笔最大支出是 2000（收入 100000 不算在内）
    TT_EQ(s.max_expense_minor.minor_units(), int64_t{2000});
    TT_EQ(s.max_expense_date, std::string("2026-09-01"));
}

TT_TEST(Summary, 区间外的记录必须被忽略) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("in1", 1000, "cat0", 2026, 9, 15),
        make_record("out1", 999999, "cat0", 2026, 8, 31),   // 上个月
        make_record("out2", 999999, "cat0", 2026, 10, 1),   // 下个月
    };
    TT_TRY_ASSIGN(s, summarize(records, cats, kSept));
    TT_EQ(s.expense_minor, int64_t{1000});
    TT_EQ(s.expense_records, int64_t{1});
}

TT_TEST(Summary, 空数据不崩溃且不除零) {
    auto cats = make_categories();
    std::vector<Record> none;
    TT_TRY_ASSIGN(s, summarize(none, cats, kSept));
    TT_EQ(s.expense_minor, int64_t{0});
    TT_EQ(s.active_days, int64_t{0});
    TT_EQ(s.avg_daily_expense_minor.minor_units(), int64_t{0});
    TT_EQ(s.by_category.size(), size_t{0});
}

TT_TEST(Summary, 区间非法直接报错) {
    auto cats = make_categories();
    std::vector<Record> none;
    // 终点早于起点
    TT_IS_ERR(summarize(none, cats, DateRange{Date{2026, 9, 30}, Date{2026, 9, 1}}));
    // 无效日期
    TT_IS_ERR(summarize(none, cats, DateRange{Date{2026, 13, 1}, Date{2026, 9, 1}}));
}

TT_TEST(Summary, 分类占比之和为一) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("r1", 3000, "cat0", 2026, 9, 1),
        make_record("r2", 1000, "cat1", 2026, 9, 2),
        make_record("r3", 6000, "cat2", 2026, 9, 3),
    };
    TT_TRY_ASSIGN(s, summarize(records, cats, kSept));
    TT_EQ(s.by_category.size(), size_t{3});

    double sum = 0.0;
    for (const auto& b : s.by_category) sum += b.share;
    TT_NEAR(sum, 1.0, 1e-9);

    // 必须按金额降序，且金额最大的是「购物」6000
    TT_EQ(s.by_category.front().category_name, std::string("购物"));
    TT_EQ(s.by_category.front().total_minor, int64_t{6000});
    TT_NEAR(s.by_category.front().share, 0.6, 1e-9);
}

TT_TEST(Summary, 日序列补零且长度等于区间天数) {
    std::vector<Record> records = {
        make_record("r1", 1000, "cat0", 2026, 9, 3),
        make_record("r2", 2000, "cat0", 2026, 9, 3),
        make_record("r3", 500, "cat0", 2026, 9, 7),
    };
    TT_TRY_ASSIGN(series, build_daily_series(records, kSept));
    TT_EQ(series.size(), size_t{30});

    // 9/1、9/2 没有记录，必须是 0 而不是被跳过
    TT_EQ(series[0].date, Date({2026, 9, 1}));
    TT_EQ(series[0].expense_minor, int64_t{0});
    TT_CHECK(!series[0].has_records);
    TT_EQ(series[2].expense_minor, int64_t{3000});
    TT_EQ(series[2].record_count, int64_t{2});
    TT_CHECK(series[2].has_records);
    TT_EQ(series[6].expense_minor, int64_t{500});
}

// -----------------------------------------------------------------------------
//  趋势
// -----------------------------------------------------------------------------

TT_TEST(Trend, 滑动平均是居中的) {
    std::vector<Record> records;
    // 只有 9/15 这一天有 7000 分，其余为 0
    records.push_back(make_record("r1", 7000, "cat0", 2026, 9, 15));

    TT_TRY_ASSIGN(t, compute_expense_trend(records, kSept, 7));
    TT_EQ(t.window, 7);
    TT_EQ(t.points.size(), size_t{30});

    // 居中窗口：9/15 是下标 14，窗口 [11,17] 覆盖 9/12~9/18，
    // 这 7 天总和 7000，平均 1000
    TT_EQ(t.points[14].moving_average_minor, int64_t{1000});
    TT_EQ(t.points[14].window_used, 7);

    // 边界收缩：下标 0 的窗口只有 [0,3] 共 4 天，和是 0
    TT_EQ(t.points[0].window_used, 4);
    // 末尾下标 29 的窗口是 [26,29] 共 4 天
    TT_EQ(t.points[29].window_used, 4);
}

TT_TEST(Trend, 环比与方向判定) {
    // 本期（9 月）比上一期（8 月）明显更高
    std::vector<Record> records = {
        make_record("a1", 10000, "cat0", 2026, 8, 10),
        make_record("a2", 10000, "cat0", 2026, 8, 20),
        make_record("b1", 20000, "cat0", 2026, 9, 10),
        make_record("b2", 10000, "cat0", 2026, 9, 20),
    };
    TT_TRY_ASSIGN(t, compute_expense_trend(records, kSept, 7));

    TT_EQ(t.compared_to_previous.previous_minor, int64_t{20000});
    TT_EQ(t.compared_to_previous.current_minor, int64_t{30000});
    TT_EQ(t.compared_to_previous.delta_minor, int64_t{10000});
    TT_CHECK(t.compared_to_previous.change_ratio.has_value());
    TT_NEAR(t.compared_to_previous.change_ratio.value(), 0.5, 1e-9);
    TT_EQ(t.compared_to_previous.direction, std::string("up"));
}

TT_TEST(Trend, 上期为零时不编造涨幅) {
    std::vector<Record> records = {
        make_record("b1", 20000, "cat0", 2026, 9, 10),
    };
    TT_TRY_ASSIGN(t, compute_expense_trend(records, kSept, 7));
    TT_EQ(t.compared_to_previous.previous_minor, int64_t{0});
    TT_CHECK(t.compared_to_previous.from_zero);
    // 关键：不能从 0 算比例（那会得出「涨了无穷倍」）
    TT_CHECK(!t.compared_to_previous.change_ratio.has_value());
    TT_EQ(t.compared_to_previous.direction, std::string("up"));
}

TT_TEST(Trend, 小幅波动应判为持平) {
    // 变化只有 1%，落在 ±3% 容忍带内，应当报持平而不是「上涨」
    std::vector<Record> records = {
        make_record("a1", 10000, "cat0", 2026, 8, 10),
        make_record("b1", 10100, "cat0", 2026, 9, 10),
    };
    TT_TRY_ASSIGN(t, compute_expense_trend(records, kSept, 7));
    TT_EQ(t.compared_to_previous.direction, std::string("flat"));
}

TT_TEST(Trend, 样本不足时明确告知) {
    std::vector<Record> records = {
        make_record("r1", 1000, "cat0", 2026, 9, 1),
        make_record("r2", 1000, "cat0", 2026, 9, 2),
    };
    TT_TRY_ASSIGN(t, compute_expense_trend(records, kSept, 7));
    TT_CHECK_MSG(!t.sufficient_data, "只有 2 天数据不该被判定为「足够」");
    TT_CHECK(t.note.find("不足") != std::string::npos);
    TT_EQ(t.days_with_records, int64_t{2});
}

TT_TEST(Trend, 窗口参数被规整为奇数且不超范围) {
    std::vector<Record> records;
    {
        TT_TRY_ASSIGN(t4, compute_expense_trend(records, kSept, 4));
        TT_EQ(t4.window, 5);      // 偶数 -> 加一
        TT_TRY_ASSIGN(t100, compute_expense_trend(records, kSept, 100));
        TT_EQ(t100.window, 31);   // 上限
        TT_TRY_ASSIGN(t0, compute_expense_trend(records, kSept, 0));
        TT_EQ(t0.window, 1);      // 下限
    }
}

TT_TEST(Trend, 分类趋势只统计该分类) {
    std::vector<Record> records = {
        make_record("r1", 1000, "cat0", 2026, 9, 1),
        make_record("r2", 5000, "cat1", 2026, 9, 1),
    };
    TT_TRY_ASSIGN(t, compute_category_trend(records, "cat0", kSept, 7));
    int64_t total = 0;
    for (const auto& p : t.points) total += p.value_minor;
    TT_EQ(total, int64_t{1000});

    // 空分类 id 必须报错
    TT_IS_ERR(compute_category_trend(records, "", kSept, 7));
}

// -----------------------------------------------------------------------------
//  异常识别
// -----------------------------------------------------------------------------

TT_TEST(Anomaly, 样本充足时用3sigma命中明显离群) {
    auto cats = make_categories();
    std::vector<Record> records;
    // 20 笔 1000 左右的日常支出，加上 1 笔 50000 的离群
    for (int i = 0; i < 20; ++i) {
        records.push_back(make_record("n" + std::to_string(i), 1000 + i * 10, "cat0",
                                      2026, 9, 1 + i));
    }
    records.push_back(make_record("outlier", 50000, "cat0", 2026, 9, 25));

    TT_TRY_ASSIGN(report, detect_anomalies(records, cats, kSept));
    TT_CHECK(report.reliable);
    TT_EQ(report.sample_size, int64_t{21});

    // 注意：同一笔离群金额可能同时触发「单笔异常」和「单日总额异常」，
    // 两者金额相同（都是 50000），所以不能只按金额匹配——必须按 kind 分辨。
    bool found_single = false;
    bool found_daily = false;
    for (const auto& a : report.items) {
        if (a.amount_minor != 50000) continue;
        if (a.kind == AnomalyKind::SingleExpense) {
            found_single = true;
            TT_CHECK(a.score > 1.0);
            TT_CHECK_MSG(!a.record_id.empty(), "单笔异常必须带上记录 id，便于界面跳转");
        } else if (a.kind == AnomalyKind::DailyTotal) {
            found_daily = true;
        }
    }
    TT_CHECK_MSG(found_single, "50000 的那笔应当被判为单笔异常");
    TT_CHECK_MSG(found_daily, "9/25 只有这一笔，单日总额也应当超标");
    // 口径与局限必须写明
    TT_CHECK(!report.method_summary.empty());
    TT_CHECK(!report.limitations.empty());
}

TT_TEST(Anomaly, 样本不足时降级为IQR并说明) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("a", 1000, "cat0", 2026, 9, 1),
        make_record("b", 1100, "cat0", 2026, 9, 2),
        make_record("c", 1200, "cat0", 2026, 9, 3),
        make_record("d", 1300, "cat0", 2026, 9, 4),
        make_record("e", 30000, "cat0", 2026, 9, 5),   // 明显离群
    };
    TT_TRY_ASSIGN(report, detect_anomalies(records, cats, kSept));
    // 只有 5 笔，不该声称可靠
    TT_CHECK_MSG(!report.reliable, "5 笔样本不该被标记为 reliable");
    // 必须说明是降级方法
    TT_CHECK(report.method_summary.find("IQR") != std::string::npos);
    // 局限里必须包含「样本偏小」这一条
    bool mentioned = false;
    for (const auto& l : report.limitations) {
        if (l.find("样本") != std::string::npos) mentioned = true;
    }
    TT_CHECK_MSG(mentioned, "样本不足时必须在 limitations 里说明");
}

TT_TEST(Anomaly, 全部金额相同时不作判定) {
    auto cats = make_categories();
    std::vector<Record> records;
    for (int i = 0; i < 15; ++i) {
        records.push_back(make_record("u" + std::to_string(i), 1500, "cat0",
                                      2026, 9, 1 + i));
    }
    TT_TRY_ASSIGN(report, detect_anomalies(records, cats, kSept));
    TT_EQ_MSG(report.items.size(), size_t{0},
              "金额完全相同时不该报异常（那是噪声不是异常）");
    TT_CHECK(report.method_summary.find("完全相同") != std::string::npos);
}

TT_TEST(Anomaly, 单日总额异常能识别) {
    auto cats = make_categories();
    std::vector<Record> records;
    // 多天每天一笔小额
    for (int i = 0; i < 12; ++i) {
        records.push_back(make_record("d" + std::to_string(i), 1000, "cat0",
                                      2026, 9, 1 + i));
    }
    // 某一天 5 笔大额，总额远超平时
    for (int i = 0; i < 5; ++i) {
        records.push_back(make_record("big" + std::to_string(i), 9000, "cat1",
                                      2026, 9, 20));
    }

    TT_TRY_ASSIGN(report, detect_anomalies(records, cats, kSept));
    bool found_daily = false;
    for (const auto& a : report.items) {
        if (a.kind == AnomalyKind::DailyTotal && a.date == Date({2026, 9, 20})) {
            found_daily = true;
        }
    }
    TT_CHECK_MSG(found_daily, "9/20 的单日总额应当被判为异常");
}

TT_TEST(Anomaly, 空数据和全收入都不报错) {
    auto cats = make_categories();
    std::vector<Record> none;
    {
        TT_TRY_ASSIGN(report, detect_anomalies(none, cats, kSept));
        TT_EQ(report.sample_size, int64_t{0});
        TT_EQ(report.items.size(), size_t{0});
        TT_CHECK(!report.limitations.empty());
    }
    {
        std::vector<Record> only_income = {
            make_record("i1", 100000, "cat3", 2026, 9, 1, Direction::Income),
        };
        TT_TRY_ASSIGN(report, detect_anomalies(only_income, cats, kSept));
        TT_EQ(report.sample_size, int64_t{0});   // 只看支出
        TT_EQ(report.items.size(), size_t{0});
    }
}

// -----------------------------------------------------------------------------
//  建议规则
// -----------------------------------------------------------------------------

TT_TEST(Insight, 集中度规则与算术节省) {
    auto cats = make_categories();
    std::vector<Record> records;
    // 餐饮占 80%
    for (int i = 0; i < 10; ++i) {
        records.push_back(make_record("f" + std::to_string(i), 8000, "cat0",
                                      2026, 9, 1 + i));
    }
    for (int i = 0; i < 5; ++i) {
        records.push_back(make_record("s" + std::to_string(i), 1000, "cat1",
                                      2026, 9, 11 + i));
    }

    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);

    bool has_concentration = false;
    bool has_arithmetic = false;
    for (const auto& i : insights.items) {
        if (i.rule_id == "concentration") {
            has_concentration = true;
            TT_EQ(i.category_name, std::string("餐饮"));
            // 每一条建议都必须带具体金额，不许只有空话
            TT_CHECK(i.related_minor > 0);
        }
        if (i.rule_id == "arithmetic_saving") {
            has_arithmetic = true;
            TT_CHECK(i.related_minor > 0);
        }
    }
    TT_CHECK_MSG(has_concentration, "餐饮占 80% 应当触发集中度提示");
    TT_CHECK_MSG(has_arithmetic, "数据充足时应当给出算术上的节省空间");

    // 局限永远不能为空——这是这个模块的底线
    TT_CHECK_MSG(insights.limitations.size() >= 3, "已知局限至少要有 3 条");
}

TT_TEST(Insight, 记录稀疏必须被点出来) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("r1", 1000, "cat0", 2026, 9, 1),
    };
    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);

    bool has_sparse = false;
    for (const auto& i : insights.items) {
        if (i.rule_id == "sparse_recording") has_sparse = true;
    }
    TT_CHECK_MSG(has_sparse, "30 天只有 1 天有记录，必须提示覆盖率过低");
    TT_CHECK_MSG(!insights.actionable, "数据不足时 actionable 必须为 false");
}

TT_TEST(Insight, 环比上升触发提醒) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("a1", 10000, "cat0", 2026, 8, 10),
        make_record("b1", 20000, "cat0", 2026, 9, 10),
    };
    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);

    bool has_mom = false;
    for (const auto& i : insights.items) {
        if (i.rule_id == "mom_up") has_mom = true;
    }
    TT_CHECK_MSG(has_mom, "支出翻倍应当触发环比提醒");
}

// -----------------------------------------------------------------------------
//  prompt 构造
// -----------------------------------------------------------------------------

TT_TEST(Prompt, 两档长度与内容差异符合预期) {
    auto cats = make_categories();
    std::vector<Record> records;
    for (int i = 0; i < 20; ++i) {
        records.push_back(make_record("r" + std::to_string(i), 1000 + i * 300,
                                      i % 2 == 0 ? "cat0" : "cat1", 2026, 9, 1 + i));
    }

    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);

    auto facts = report::build_facts(report::ReportKind::Monthly, dash, trend, anomalies, insights);

    auto compact = report::build_prompt(facts, report::PromptStyle::Compact);
    auto detailed = report::build_prompt(facts, report::PromptStyle::Detailed);

    // 本地小模型那档必须明显更短
    TT_CHECK_MSG(compact.system.size() < detailed.system.size(),
                 "Compact 的系统提示应当比 Detailed 短");
    TT_CHECK_MSG(compact.user.size() < detailed.user.size(),
                 "Compact 的用户提示应当比 Detailed 短");
    TT_CHECK_MSG(compact.user.size() < 1200,
                 "给 440M 量级本地模型的 prompt 不该超过约 1200 字符，实际 " +
                     std::to_string(compact.user.size()));

    // 两档都必须禁止编造数据
    TT_CHECK(compact.system.find("不能编") != std::string::npos ||
             compact.system.find("只能使用") != std::string::npos);
    TT_CHECK(detailed.system.find("禁止推算") != std::string::npos ||
             detailed.system.find("只使用给定数字") != std::string::npos);

    // 关键数字必须出现在 user prompt 里（界面显示的数字和喂给模型的一致）
    const std::string total = Money::from_minor(facts.expense_minor).to_plain_string();
    TT_CHECK_MSG(compact.user.find(total) != std::string::npos,
                 "compact prompt 里应当出现总支出 " + total);
    TT_CHECK_MSG(detailed.user.find(total) != std::string::npos,
                 "detailed prompt 里应当出现总支出 " + total);

    // Detailed 必须带上局限说明，否则模型不会提
    if (!facts.limitations.empty()) {
        TT_CHECK(detailed.user.find("已知局限") != std::string::npos);
    }
}

TT_TEST(Prompt, facts序列化是合法JSON且数字一致) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("r1", 12345, "cat0", 2026, 9, 1),
    };
    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);
    auto facts = report::build_facts(report::ReportKind::Daily, dash, trend, anomalies, insights);

    const std::string text = report::facts_to_json(facts);
    TT_CHECK(!text.empty());
    TT_CHECK(text.find("\"expense_minor\": 12345") != std::string::npos);
    TT_CHECK(text.find("\"kind\": \"daily\"") != std::string::npos);
}

TT_TEST(Prompt, 报告类型与区间对应) {
    const Date anchor{2026, 9, 18};

    auto daily = report::range_for(report::ReportKind::Daily, anchor);
    TT_EQ(daily.from, Date({2026, 9, 18}));
    TT_EQ(daily.to, Date({2026, 9, 18}));

    auto weekly = report::range_for(report::ReportKind::Weekly, anchor);
    TT_EQ(weekly.from, Date({2026, 9, 14}));   // 周一
    TT_EQ(weekly.to, Date({2026, 9, 20}));     // 周日

    auto monthly = report::range_for(report::ReportKind::Monthly, anchor);
    TT_EQ(monthly.from, Date({2026, 9, 1}));
    TT_EQ(monthly.to, Date({2026, 9, 30}));

    TT_EQ(std::string(report::storage_id(report::ReportKind::Weekly)), std::string("weekly"));
    TT_CHECK(report::parse_kind("monthly").value() == report::ReportKind::Monthly);
    TT_CHECK(report::parse_kind("月").value() == report::ReportKind::Monthly);
    TT_CHECK(!report::parse_kind("hourly").has_value());
}

TT_TEST(Prompt, 规则模板兜底必须含数字与局限) {
    auto cats = make_categories();
    std::vector<Record> records = {
        make_record("r1", 5000, "cat0", 2026, 9, 1),
        make_record("r2", 3000, "cat1", 2026, 9, 2),
    };
    TT_TRY_ASSIGN(dash, build_dashboard(records, cats, kSept));
    TT_TRY_ASSIGN(trend, compute_expense_trend(records, kSept, 7));
    TT_TRY_ASSIGN(anomalies, detect_anomalies(records, cats, kSept));
    auto insights = generate_insights(dash, trend, anomalies);
    auto facts = report::build_facts(report::ReportKind::Monthly, dash, trend, anomalies, insights);

    // render_rule_based 在 report_service 里，这里只验 facts 足够支撑模板
    TT_EQ(facts.expense_minor, int64_t{8000});
    TT_EQ(facts.expense_records, int64_t{2});
    TT_CHECK(!facts.categories.empty());
    TT_CHECK(!facts.limitations.empty());
    TT_CHECK(!facts.data_note.empty());
    TT_CHECK(facts.range.from == Date({2026, 9, 1}));
}
