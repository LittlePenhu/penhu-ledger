// =============================================================================
//  native/pages/stats.cpp
//  统计页：总览 + 日趋势 + 分类占比 + 异常提示。
// =============================================================================

#include <algorithm>
#include <cmath>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "penhu/domain/money.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
using namespace ui;

/// 大金额展示块（金额 + 标签 + 环比一句）
class BigAmount : public Widget {
public:
    std::wstring amount;
    std::wstring label;
    std::wstring delta;          // 环比说明，可空
    Color        amount_color{0x00000000u};
    bool         delta_good{false};

    float preferred_height(float, Renderer&) override { return delta.empty() ? 76.0f : 100.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        const Color c = color_unset(amount_color) ? th.palette.on_surface : amount_color;
        r.text_line(amount, {bounds_.x, bounds_.y + 2.0f, bounds_.w, 48.0f}, TypeStyle::DisplaySmall,
                    c, TextAlign::Left, true);
        r.text_line(label, {bounds_.x + 2.0f, bounds_.y + 52.0f, bounds_.w, 20.0f},
                    TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, true);
        if (!delta.empty()) {
            const Color dc = delta_good ? th.palette.good : th.palette.warn;
            r.text_line(delta, {bounds_.x + 2.0f, bounds_.y + 74.0f, bounds_.w, 20.0f},
                        TypeStyle::BodySmall, dc, TextAlign::Left, true);
        }
    }
};

/// 单条异常 / 建议
class AnomalyRow : public Widget {
public:
    std::wstring title;
    std::wstring detail;
    int          severity{1};   // 0 good / 1 info / 2 warn
    std::wstring amount;

    float preferred_height(float width, Renderer& r) override {
        (void)width;
        (void)r;
        return detail.empty() ? 44.0f : 62.0f;
    }
    void layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        const Color dot = (severity == 2) ? th.palette.warn
                                          : (severity == 0 ? th.palette.good : th.palette.primary);
        r.fill_circle(bounds_.x + 6.0f, bounds_.y + 14.0f, 4.0f, dot);

        const float right_w = amount.empty() ? 0.0f : 110.0f;
        const float text_w = std::max(60.0f, bounds_.w - 26.0f - right_w);

        r.text_line(title, {bounds_.x + 18.0f, bounds_.y + 2.0f, text_w, 22.0f},
                    TypeStyle::BodyMedium, th.palette.on_surface, TextAlign::Left, true);
        if (!detail.empty()) {
            r.text_line(detail, {bounds_.x + 18.0f, bounds_.y + 24.0f, text_w, 20.0f},
                        TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, true);
        }
        if (!amount.empty()) {
            r.text_line(amount, {bounds_.right() - right_w, bounds_.y + 2.0f, right_w, 22.0f},
                        TypeStyle::BodyMedium, th.palette.on_surface, TextAlign::Right, true);
        }
    }
};

std::wstring fmt_minor(int64_t minor) {
    return ui::to_wide(Money::from_minor(minor).to_grouped_string());
}

}  // namespace

void build_stats_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = page_padding(app);
    col.gap = 16.0f;
    col.max_width = app.content_max_width();

    // 统计口径下沉成一行小字（原来的页头副标题）
    info_line(col, th, ui::to_wide(app.data_note));


    // 宽屏把「日趋势」和「分类占比」并排（2:1）。这两块是同一个问题的两个视角 ——
    // 「钱花在哪些天」和「钱花在哪些类」，并排看更容易对上。
    // 窄屏时两个指针都指向同一列，按代码顺序堆叠，与原来完全一致。
    VBox* trend_col = &col;
    VBox* pie_col = &col;

    // ---------------- 区间 ----------------
    auto* range = col.emplace<SegmentedTabs>();
    range->items = {L"近 7 天", L"近 30 天", L"近 90 天"};
    range->selected = (app.range_days <= 7) ? 0 : (app.range_days <= 30 ? 1 : 2);
    range->on_change = [&app](int idx) {
        const int days = (idx == 0) ? 7 : (idx == 1 ? 30 : 90);
        if (days == app.range_days) return;
        app.range_days = days;
        app.refresh();
        app.rebuild_ui();
    };

    const Summary& s = app.dashboard.summary;

    // ---------------- 总览 ----------------
    auto* overview = col.emplace<Card>();
    overview->padding = Insets::all(20.0f);
    overview->gap = 18.0f;

    auto* big = overview->emplace<BigAmount>();
    big->amount = fmt_minor(s.expense_minor);
    big->label = L"区间总支出（" + std::to_wstring(app.range_days) + L" 天）";
    big->amount_color = th.palette.expense;

    const auto& cmp = app.trend.compared_to_previous;
    if (cmp.previous_minor > 0 && cmp.direction != "flat") {
        const long long pct = static_cast<long long>(
            std::llround((cmp.change_ratio.value_or(0.0)) * 100.0));
        big->delta = (cmp.direction == "up" ? L"比上一周期多花 " : L"比上一周期少花 ") +
                     std::to_wstring(pct < 0 ? -pct : pct) + L"%（上一周期 " +
                     fmt_minor(cmp.previous_minor) + L"）";
        big->delta_good = (cmp.direction == "down");
    } else if (cmp.from_zero) {
        big->delta = L"上一周期没有支出记录，无法比较";
        big->delta_good = true;
    } else if (!app.trend.sufficient_data) {
        big->delta = L"数据太少，暂不做环比（" + ui::to_wide(app.trend.note) + L"）";
        big->delta_good = true;
    }

    auto* stats_row = overview->emplace<HBox>();
    stats_row->gap = 12.0f;
    stats_row->align = HBox::Align::Stretch;

    auto* c1 = stats_row->emplace<StatCell>();
    c1->label = L"区间收入";
    c1->value = fmt_minor(s.income_minor);
    c1->value_color = th.palette.income;

    auto* c2 = stats_row->emplace<StatCell>();
    c2->label = L"结余";
    c2->value = fmt_minor(s.net_minor);
    c2->value_color = (s.net_minor >= 0) ? th.palette.income : th.palette.expense;

    auto* c3 = stats_row->emplace<StatCell>();
    c3->label = L"日均支出";
    c3->value = fmt_minor(s.avg_daily_expense_minor.minor_units());

    auto* c4 = stats_row->emplace<StatCell>();
    c4->label = L"笔数（支出/收入）";
    c4->value = std::to_wstring(s.expense_records) + L" / " + std::to_wstring(s.income_records);

    if (s.expense_records == 0) {
        auto* empty = overview->emplace<InfoNote>();
        empty->text = L"这个区间还没有支出记录。去「记账」页记一笔，统计就有内容了。";
    }

    if (app.wide()) {
        auto* row = col.emplace<HBox>();
        row->gap = 20.0f;
        row->align = HBox::Align::Start;
        auto* l = row->emplace<VBox>();
        l->flex = 2.0f;   // 趋势图要宽：它横向信息量更大，占比给 1 份够了
        l->gap = 16.0f;
        auto* r = row->emplace<VBox>();
        r->flex = 1.0f;
        r->gap = 16.0f;
        trend_col = l;
        pie_col = r;
    }

    // ---------------- 日趋势 ----------------
    if (!app.dashboard.daily.empty()) {
        auto* t_head = trend_col->emplace<SectionTitle>();
        t_head->title = L"每日支出";
        t_head->trailing = L"柱高按区间内最大值归一";

        auto* t_card = trend_col->emplace<Card>();
        t_card->padding = Insets::all(16.0f);
        t_card->gap = 10.0f;

        auto* chart = t_card->emplace<BarChart>();
        chart->bar = th.palette.primary;
        chart->bar_alt = th.palette.tertiary;
        chart->height = 150.0f;

        int64_t peak = 0;
        for (const auto& d : app.dashboard.daily) peak = std::max(peak, d.expense_minor);
        for (const auto& d : app.dashboard.daily) {
            chart->values.push_back(peak > 0 ? static_cast<float>(d.expense_minor) /
                                                   static_cast<float>(peak)
                                             : 0.0f);
        }

        // 标签只在点够少时全画；90 天时每 7 天一个，否则会糊成一片
        const size_t n = app.dashboard.daily.size();
        const size_t step = (n <= 14) ? 1 : (n / 12 + 1);
        for (size_t i = 0; i < n; ++i) {
            if (i % step != 0 && i + 1 != n) {
                chart->labels.emplace_back();
                continue;
            }
            const Date& d = app.dashboard.daily[i].date;
            chart->labels.push_back(std::to_wstring(d.month) + L"/" + std::to_wstring(d.day));
        }
        chart->highlight_last = static_cast<int>(n) - 1;

        auto* note = t_card->emplace<Label>();
        note->text = L"最高的一天：" + fmt_minor(peak) + L"　·　" +
                     ui::to_wide(app.trend.note);
        note->style = TypeStyle::BodySmall;
        note->color = th.palette.on_surface_variant;
    }

    // ---------------- 分类占比 ----------------
    auto* b_head = pie_col->emplace<SectionTitle>();
    b_head->title = L"支出分类占比";
    b_head->trailing = L"按金额降序（前 8 项）";

    auto* b_card = pie_col->emplace<Card>();
    b_card->padding = Insets::all(18.0f);
    b_card->gap = 14.0f;

    std::vector<CategoryBreakdown> cats = s.by_category;
    std::sort(cats.begin(), cats.end(), [](const CategoryBreakdown& a, const CategoryBreakdown& b) {
        return a.total_minor > b.total_minor;
    });

    if (cats.empty()) {
        auto* none = b_card->emplace<Label>();
        none->text = L"该区间没有支出，所以没有占比可算。";
        none->style = TypeStyle::BodySmall;
        none->color = th.palette.on_surface_variant;
    } else {
        auto* bar = b_card->emplace<ShareBar>();
        bar->height = 14.0f;
        for (const auto& c : cats) {
            bar->segments.push_back(
                {static_cast<float>(c.total_minor), Color::from_hex(c.category_color, th.palette.primary)});
        }

        int shown = 0;
        for (const auto& c : cats) {
            if (shown >= 8) break;
            ++shown;
            auto* row = b_card->emplace<CategoryBar>();
            row->name = ui::to_wide(c.category_name);
            row->amount = fmt_minor(c.total_minor);
            row->share = static_cast<float>(c.share);
            row->color = Color::from_hex(c.category_color, th.palette.primary);
        }
    }

    // ---------------- 建议与异常 ----------------
    const auto& ins = app.insights;
    const auto& an = app.anomalies;

    if (!ins.items.empty() || !an.items.empty() || !ins.limitations.empty() || !ins.actionable) {
        auto* a_head = col.emplace<SectionTitle>();
        a_head->title = L"值得注意";
        a_head->trailing = ins.actionable ? L"" : L"数据不够，仅供参考";

        auto* a_card = col.emplace<Card>();
        a_card->padding = Insets::all(16.0f);
        a_card->gap = 6.0f;

        int shown = 0;

        // 规则建议（带严重度）
        for (const auto& item : ins.items) {
            if (shown >= 5) break;
            ++shown;
            auto* row = a_card->emplace<AnomalyRow>();
            row->title = ui::to_wide(item.title);
            row->detail = ui::to_wide(item.detail);
            row->amount = (item.related_minor != 0) ? fmt_minor(item.related_minor) : std::wstring{};
            row->severity = static_cast<int>(item.severity);
        }

        // 异常单笔：这是「统计判定」而不是「规则建议」，两者来源不同，分开列
        for (size_t i = 0; i < an.items.size() && i < 3; ++i) {
            const auto& a = an.items[i];
            auto* row = a_card->emplace<AnomalyRow>();
            row->title = ui::to_wide("单笔偏高 · " +
                                     (a.category_name.empty() ? std::string("未分类")
                                                              : a.category_name));
            row->detail = ui::to_wide(a.explanation);
            row->amount = fmt_minor(a.amount_minor);
            row->severity = 2;
            ++shown;
        }

        if (shown == 0) {
            auto* none = a_card->emplace<Label>();
            none->text = L"这个区间没有触发任何提醒规则。";
            none->style = TypeStyle::BodySmall;
            none->color = th.palette.on_surface_variant;
        }

        // 口径与盲区必须显示：这套判定是「与自身历史比」的统计规则，
        // 样本少的时候结论不可靠，藏着不说等于误导。
        std::wstring lim;
        for (size_t i = 0; i < ins.limitations.size() && i < 3; ++i) {
            if (!lim.empty()) lim += L"；";
            lim += ui::to_wide(ins.limitations[i]);
        }
        auto* note = a_card->emplace<InfoNote>();
        note->warn = !ins.actionable || !an.reliable;
        note->text = (ins.actionable ? std::wstring(L"判定口径：")
                                     : std::wstring(L"数据还不够支撑结论，先当参考。判定口径：")) +
                     (lim.empty() ? std::wstring(L"与自身历史分布比较") : lim);
    }

    // ---------------- 口径说明 ----------------
    auto* foot = col.emplace<InfoNote>();
    foot->text = ui::to_wide("口径：" + app.data_note +
                             "。金额一律以「分」为单位做整数累加，分类占比按四舍五入显示，"
                             "各项相加可能与 100% 差 1%。");
}

}  // namespace penhu::native
