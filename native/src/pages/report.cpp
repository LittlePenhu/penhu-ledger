// =============================================================================
//  native/pages/report.cpp
//  报告页：选周期 → 生成 → 读正文，并把「降级原因」如实展示。
// =============================================================================

#include <algorithm>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
using namespace ui;

/// 报告正文块。等宽感不需要，用正文字体 + 1.75 倍行高，长文更好读。
class Article : public Widget {
public:
    std::wstring text;
    std::wstring meta;

    float preferred_height(float width, Renderer& r) override {
        if (text.empty()) return 0.0f;
        const float body = std::max(80.0f, width - 36.0f);
        float h = r.measure(text, TypeStyle::BodyLarge, body, true).h;
        h += 24.0f;   // 上下内边距
        if (!meta.empty()) h += 22.0f;
        return h;
    }
    void layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        if (text.empty()) return;
        float y = bounds_.y + 16.0f;
        if (!meta.empty()) {
            r.text_line(meta, {bounds_.x + 18.0f, y, bounds_.w - 36.0f, 20.0f}, TypeStyle::BodySmall,
                        th.palette.on_surface_variant, TextAlign::Left, true);
            y += 22.0f;
        }
        r.text(text, {bounds_.x + 18.0f, y, bounds_.w - 36.0f, bounds_.bottom() - y - 12.0f},
               TypeStyle::BodyLarge, th.palette.on_surface, TextAlign::Left, VAlign::Top, true, false);
    }
};

/// 一行「通道尝试记录」
class NoteRow : public Widget {
public:
    std::wstring text;
    bool ok{false};

    float preferred_height(float width, Renderer& r) override {
        return std::max(34.0f, r.measure(text, TypeStyle::BodySmall, std::max(60.0f, width - 30.0f),
                                         true).h + 12.0f);
    }
    void layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        r.fill_circle(bounds_.x + 6.0f, bounds_.y + bounds_.h * 0.5f, 3.5f,
                      ok ? th.palette.good : th.palette.outline);
        r.text(text, {bounds_.x + 18.0f, bounds_.y + 5.0f, bounds_.w - 24.0f, bounds_.h - 10.0f},
               TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, VAlign::Middle,
               true, true);
    }
};

}  // namespace

void build_report_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    // 页头撤掉。副标题里那句「用了哪条通道」页内本来就有一份
    // （报告卡片上写着 provider），所以这里不需要再补一行。
    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = page_padding(app);
    col.gap = 16.0f;
    col.max_width = app.content_max_width();

    // 宽屏双栏：左边「怎么生成」，右边「生成出来的正文」。
    // 报告正文通常很长，和生成控件挤在一列里会让人反复滚动找按钮。
    VBox* left = &col;
    VBox* right = &col;
    if (app.wide()) {
        auto* row = col.emplace<HBox>();
        row->gap = 20.0f;
        row->align = HBox::Align::Start;
        auto* l = row->emplace<VBox>();
        l->flex = 2.0f;      // 控制区窄
        l->gap = 16.0f;
        auto* r = row->emplace<VBox>();
        r->flex = 3.0f;      // 正文宽
        r->gap = 16.0f;
        left = l;
        right = r;
    }

    // ---------------- 控制区 ----------------
    auto* controls = left->emplace<Card>();
    controls->padding = Insets::all(16.0f);
    controls->gap = 12.0f;

    auto* kind = controls->emplace<SegmentedTabs>();
    kind->items = {L"日报", L"周报", L"月报"};
    kind->selected = app.report_kind;
    kind->on_change = [&app](int idx) {
        if (app.report_kind == idx) return;
        app.report_kind = idx;
        app.report_text.clear();   // 换了周期，旧的正文对不上，先清掉
        app.report_notes.clear();
        app.rebuild_ui();
    };

    auto* buttons = controls->emplace<HBox>();
    buttons->gap = 12.0f;
    buttons->align = HBox::Align::Stretch;

    auto* gen = buttons->emplace<Button>();
    gen->flex = 2.0f;
    gen->variant = Button::Variant::Filled;
    gen->label = L"生成报告";
    gen->enabled = !app.busy;
    gen->on_click = [&app]() { app.generate_report_async(false); };

    auto* regen = buttons->emplace<Button>();
    regen->flex = 1.0f;
    regen->variant = Button::Variant::Outlined;
    regen->label = L"重新生成";
    regen->enabled = !app.busy;
    regen->on_click = [&app]() { app.generate_report_async(true); };

    auto* hint = controls->emplace<Label>();
    hint->text = app.report_kind == 0 ? L"日报按今天汇总；已有当天的存档时会直接读存档（快）。"
               : app.report_kind == 1 ? L"周报按本周（周一起算）汇总。"
                                      : L"月报按本月汇总，适合月底复盘。";
    hint->style = TypeStyle::BodySmall;
    hint->color = th.palette.on_surface_variant;

    // ---------------- 降级 / 存档提示 ----------------
    if (app.report_fallback) {
        auto* warn = left->emplace<InfoNote>();
        warn->warn = true;
        warn->text = L"这份正文是**模板兜底**生成的（没有任何模型通道可用），"
                     L"数字来自统计、文字是套模板的。要模型写的版本，请到「设置」配好模型通道后"
                     L"点「重新生成」。";
    }
    if (app.report_cache) {
        auto* cache = left->emplace<InfoNote>();
        cache->text = L"这份正文来自本地存档（当时生成的结果），没有重新调用模型。"
                      L"想要最新的一份，点「重新生成」。";
    }

    // ---------------- 正文 ----------------
    if (app.report_text.empty()) {
        auto* empty_card = right->emplace<Card>();
        empty_card->padding = Insets::all(20.0f);
        empty_card->gap = 10.0f;

        auto* t = empty_card->emplace<Label>();
        t->text = L"还没有报告";
        t->style = TypeStyle::TitleMedium;

        auto* d = empty_card->emplace<Label>();
        d->text = L"点上面的「生成报告」就会把这一天的收支汇总交给模型，"
                  L"让它写一段总结。如果还没配模型通道，正文会是模板兜底版本 —— "
                  L"数字一定对，只是文字比较朴素。";
        d->style = TypeStyle::BodySmall;
        d->color = th.palette.on_surface_variant;
    } else {
        auto* card = right->emplace<Card>();
        card->padding = Insets::all(0.0f);

        auto* article = card->emplace<Article>();
        article->text = ui::to_wide(app.report_text);
        article->meta = ui::to_wide(app.report_provider + (app.report_cache ? "（存档）" : ""));
    }

    // ---------------- 通道尝试记录 ----------------
    if (!app.report_notes.empty()) {
        auto* head = right->emplace<SectionTitle>();
        head->title = L"模型通道尝试记录";
        head->trailing = L"降级链的每一步都在这里";

        auto* card = right->emplace<Card>();
        card->padding = Insets::all(16.0f);
        card->gap = 4.0f;

        for (const auto& n : app.report_notes) {
            auto* row = card->emplace<NoteRow>();
            row->text = ui::to_wide(n);
            row->ok = n.find("成功") != std::string::npos || n.find("ok") != std::string::npos;
        }

        auto* foot = card->emplace<Label>();
        foot->text = L"失败不等于出错：本地模型没启动、云端 Key 没配，都会走到模板兜底，"
                     L"这样你至少不会打开一个空白页。";
        foot->style = TypeStyle::BodySmall;
        foot->color = th.palette.on_surface_variant;
    }
}

}  // namespace penhu::native
