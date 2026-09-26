// =============================================================================
//  native/pages/scan.cpp
//  截图记账页：选图 → 识别 → 核对 → 入账。
//
//  这一页的交互顺序是刻意设计的：识别结果**不允许直接落库**。
//  模型读到的东西必须经过人眼确认，因为它有可能会把「余额」当成「金额」、
//  把退款当成消费。一个错金额静默进账本，比让用户多点一次「确认」糟糕得多。
// =============================================================================

#include <algorithm>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "penhu/domain/money.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
using namespace ui;

/// 图片预览。首次绘制时解码，之后复用。
class ImagePreview : public Widget {
public:
    std::wstring path;
    std::wstring placeholder;
    /// 预览高度。识别页把它压到 140dp ——
    /// 240dp 的大图会把「金额 / 分类」这些真正要核对的东西推出首屏，
    /// 而用户在这一页想看的是数字对不对，不是把原图再欣赏一遍。
    float height{240.0f};

    float preferred_height(float, Renderer&) override { return height; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        r.fill_round(bounds_, Theme::kCornerMd, th.palette.surface_container);

        if (path.empty()) {
            r.text_line(placeholder, bounds_.inset(20.0f), TypeStyle::BodySmall,
                        th.palette.on_surface_variant, TextAlign::Center, true);
            return;
        }
        if (!tried_) {
            tried_ = true;
            decoded_ok_ = r.load_image(path, image_);
        }
        if (decoded_ok_ && bitmap_ok(image_)) {
            r.draw_image(bitmap_raw(image_), bounds_.inset(10.0f), Theme::kCornerSm, false);
        } else {
            r.text_line(L"这张图预览不了（格式可能不被支持），但仍然可以尝试识别。",
                        bounds_.inset(20.0f), TypeStyle::BodySmall, th.palette.on_surface_variant,
                        TextAlign::Center, true);
        }
    }

private:
    BitmapRef image_;
    bool tried_{false};
    bool decoded_ok_{false};
};

/// 「字段：值」一行
class KeyValueRow : public Widget {
public:
    std::wstring key;
    std::wstring value;
    Color        value_color{0x00000000u};

    float preferred_height(float, Renderer&) override { return 34.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        r.text_line(key, {bounds_.x, bounds_.y, 96.0f, bounds_.h}, TypeStyle::BodySmall,
                    th.palette.on_surface_variant, TextAlign::Left, false);
        const Color c = color_unset(value_color) ? th.palette.on_surface : value_color;
        r.text_line(value, {bounds_.x + 104.0f, bounds_.y, bounds_.w - 104.0f, bounds_.h},
                    TypeStyle::BodyMedium, c, TextAlign::Left, true);
    }
};

/// 置信度条
class ConfidenceBar : public Widget {

public:
    float value{0.0f};

    float preferred_height(float, Renderer&) override { return 34.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        const float pct = std::clamp(value, 0.0f, 1.0f);
        const Color c = (pct >= 0.8f) ? th.palette.good
                                      : (pct >= 0.6f ? th.palette.primary : th.palette.warn);

        r.text_line(L"金额把握", {bounds_.x, bounds_.y, 72.0f, bounds_.h}, TypeStyle::BodySmall,
                    th.palette.on_surface_variant, TextAlign::Left, false);

        const Rect track{bounds_.x + 80.0f, bounds_.y + (bounds_.h - 8.0f) * 0.5f,
                         std::max(20.0f, bounds_.w - 160.0f), 8.0f};
        r.fill_round(track, 4.0f, th.palette.surface_container_highest);
        const float w = std::max(0.0f, track.w * pct);
        if (w > 0.5f) r.fill_round({track.x, track.y, w, track.h}, 4.0f, c);

        r.text_line(std::to_wstring(static_cast<int>(pct * 100.0f + 0.5f)) + L"%",
                    {bounds_.right() - 70.0f, bounds_.y, 70.0f, bounds_.h}, TypeStyle::BodySmall,
                    c, TextAlign::Right, false);
    }
};

/// 图片列表里的一行。多图模式下这是「总览」：一眼看清哪几张好了、
/// 哪张还在识别、哪张已经入了账 —— 逐张点开才看细节。
class ScanRow : public Widget {
public:
    int          index{0};
    std::wstring label;
    std::wstring dims;
    int          state{0};        // 对应 ScanItem::State 的序号
    std::wstring summary;         // 金额或失败原因（截断显示）
    bool         selected{false};
    bool         applied{false};

    std::function<void()> on_open;
    std::function<void()> on_drop;

    ScanRow() { focusable = true; }

    float preferred_height(float, Renderer&) override { return 62.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    bool on_pointer_down(const Point&) override {
        pressed_ = true;
        return true;
    }
    bool on_pointer_move(const Point& p, bool inside) override {
        hover_ = inside;
        return false;   // 让父容器照常处理（滚轮/滚动条）
    }
    bool on_pointer_up(const Point& p) override {
        const bool was = pressed_;
        pressed_ = false;
        if (!was) return false;
        // 点右侧的 × 是「移除」，点别处是「打开这张」
        const Rect drop = drop_rect();
        if (drop.w > 0.0f && drop.contains(p)) {
            if (on_drop) on_drop();
        } else if (on_open) {
            on_open();
        }
        return true;
    }

private:
    Rect drop_rect() const { return {bounds_.right() - 44.0f, bounds_.y, 44.0f, bounds_.h}; }

    bool pressed_{false};
    bool hover_{false};

public:
    void paint(Renderer& r, const Theme& th) override {
        const Color bg = selected ? th.palette.secondary_container
                                  : (hover_ ? th.palette.surface_container_high
                                            : th.palette.surface_container_low);
        r.fill_round(bounds_, Theme::kCornerSm, bg);
        if (selected) r.stroke_round(bounds_, Theme::kCornerSm, th.palette.primary, 2.0f);

        // 左侧状态圆点：颜色就是状态本身
        Color dot = th.palette.on_surface_variant;
        switch (state) {
            case 1: dot = th.palette.primary; break;                       // 识别中
            case 2: dot = applied ? th.palette.good : th.palette.tertiary; break;  // 已识别
            case 3: dot = th.palette.error; break;                         // 失败
            default: dot = th.palette.outline; break;                      // 待识别
        }
        r.fill_circle(bounds_.x + 22.0f, bounds_.center_y(), 6.0f, dot);

        const float text_x = bounds_.x + 40.0f;
        const float text_w = std::max(40.0f, bounds_.w - 40.0f - 100.0f);

        r.text_line(std::to_wstring(index + 1) + L". " + label,
                    {text_x, bounds_.y + 8.0f, text_w, 22.0f}, TypeStyle::BodyMedium,
                    th.palette.on_surface, TextAlign::Left, false);

        std::wstring sub = dims;
        if (!summary.empty()) sub += (sub.empty() ? L"" : L"　·　") + summary;
        r.text_line(sub, {text_x, bounds_.y + 32.0f, text_w, 20.0f}, TypeStyle::BodySmall,
                    state == 3 ? th.palette.error : th.palette.on_surface_variant,
                    TextAlign::Left, false);

        // 右侧 ×：移除这一张
        const Rect drop = drop_rect();
        if (hover_) {
            r.text_line(L"✕", drop, TypeStyle::TitleMedium, th.palette.on_surface_variant,
                        TextAlign::Center, true);
        }
    }
};

/// 把「分」转成输入框里的文本。**不带千分位**：
/// 千分位是给人读的，放进输入框会让「12,345.00」既难改又容易被解析错。
std::wstring amount_edit_text(int64_t minor) {
    const int64_t a = minor < 0 ? -minor : minor;
    std::wstring cents = std::to_wstring(a % 100);
    if (cents.size() < 2) cents = L"0" + cents;
    return std::to_wstring(a / 100) + L"." + cents;
}

}  // namespace

void build_scan_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    // 先把这批图的账算清楚，页头一句话就能说明白「现在什么状态」
    int pending = 0, ready_n = 0, applied_n = 0, failed_n = 0;
    for (const ScanItem& it : app.scan_items) {
        if (it.applied) { ++applied_n; continue; }
        if (it.draft.ready) ++ready_n;
        else if (it.state == ScanItem::State::Failed) ++failed_n;
        else ++pending;
    }

    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = page_padding(app);
    col.gap = 16.0f;
    col.max_width = app.content_max_width();

    // 这一批的进度：张数、已入账、待确认、待识别 —— 原来的页头副标题
    info_line(col, th,
              app.scan_items.empty()
                  ? (app.scan_dir.empty()
                         ? std::wstring(L"选一张或多张支付截图，模型读出金额和商户，你确认后入账")
                         : (L"把截图放进 " + short_path(app.scan_dir) + L"，按「扫描目录」批量导入"))
                  : (L"共 " + std::to_wstring(app.scan_items.size()) + L" 张　已入账 " +
                     std::to_wstring(applied_n) + L"　待确认 " + std::to_wstring(ready_n) +
                     L"　待识别 " + std::to_wstring(pending + failed_n)));


    // 宽屏双栏：左边选图 + 这一批的列表，右边是选中那张的详情。
    // 批量处理最需要的就是「列表与详情同时可见」——
    // 单栏下每改一张都要滚上滚下，五张图就能把耐心耗光。
    VBox* left = &col;
    VBox* right = &col;
    if (app.wide()) {
        auto* row = col.emplace<HBox>();
        row->gap = 20.0f;
        row->align = HBox::Align::Start;
        auto* l = row->emplace<VBox>();
        l->flex = 4.0f;      // 列表窄一些
        l->gap = 16.0f;
        auto* r = row->emplace<VBox>();
        r->flex = 5.0f;      // 详情（含预览图）宽一些
        r->gap = 16.0f;
        left = l;
        right = r;
    }

    // ---------------- 选图与批量操作 ----------------
    auto* pick_card = left->emplace<Card>();
    pick_card->padding = Insets::all(16.0f);
    pick_card->gap = 12.0f;

    auto* pick_row = pick_card->emplace<HBox>();
    pick_row->gap = 12.0f;
    pick_row->align = HBox::Align::Center;

    auto* pick = pick_row->emplace<Button>();
    pick->variant = Button::Variant::Tonal;
    pick->label = app.scan_items.empty() ? L"选择截图" : L"再加几张";
    pick->glyph = L"▣";
    pick->min_height = 48.0f;
    pick->block = false;
    // 指定宽度而不是让 HBox 平分：短标签按钮被平分后会是半个屏宽的色块
    pick->width_hint = 148.0f;
    pick->on_click = [&app]() {
        app.pick_screenshot();
        app.rebuild_ui();
    };

    auto* scan_dir_btn = pick_row->emplace<Button>();
    scan_dir_btn->variant = Button::Variant::Tonal;
    scan_dir_btn->label = L"扫描目录";
    scan_dir_btn->glyph = L"⟳";
    scan_dir_btn->min_height = 48.0f;
    scan_dir_btn->block = false;
    scan_dir_btn->width_hint = 148.0f;
    scan_dir_btn->enabled = !app.scan_dir.empty() && !app.busy;
    scan_dir_btn->on_click = [&app]() {
        const int n = app.scan_directory();
        if (n == 0) {
            app.show_toast(L"目录里没有新的截图（已处理过的会跳过）", false);
        } else {
            app.show_toast(L"发现 " + std::to_wstring(n) + L" 张新截图", false);
            app.scan_async();   // 扫到就直接开始识别，省一次点击
        }
        app.rebuild_ui();
    };

    auto* info = pick_row->emplace<Label>();
    info->flex = 1.0f;
    // 单行 + 省略号：这一行要显示目录路径，换行会把它撑成两行、挤掉下面的按钮行
    info->wrap = false;
    info->ellipsis = true;
    if (app.scan_items.empty()) {
        info->text = app.scan_dir.empty()
                         ? std::wstring(L"支持 PNG / JPG / BMP / WEBP / TIFF，一次可以选多张")
                         : (L"目录：" + short_path(app.scan_dir));
        info->color = th.palette.on_surface_variant;
    } else {
        info->text = L"已选 " + std::to_wstring(app.scan_items.size()) + L" 张" +
                     (pending > 0 ? (L"，其中 " + std::to_wstring(pending) + L" 张还没识别")
                                  : std::wstring(L""));
        info->color = th.palette.on_surface;
    }
    info->style = TypeStyle::BodySmall;
    info->valign = VAlign::Middle;

    if (!app.scan_items.empty()) {
        auto* act_row = pick_card->emplace<HBox>();
        act_row->gap = 12.0f;
        act_row->align = HBox::Align::Stretch;

        auto* run = act_row->emplace<Button>();
        run->flex = 1.0f;
        run->variant = Button::Variant::Filled;
        run->label = app.busy ? L"识别中…"
                              : (L"开始识别" + (pending > 0 ? L"（" + std::to_wstring(pending) + L" 张）"
                                                           : std::wstring(L"")));
        run->glyph = L"◫";
        // 识别中禁用是防重复提交，不是「锁住整个界面」——
        // 期间切页面、翻记录、看统计都不受影响。
        run->enabled = !app.busy && pending > 0;
        // 这里曾经忘了接 on_click —— 按钮画得出来、点得下去，但永远不会有反应。
        // 「扫描目录」那条路会自动触发识别（省一次点击），把这个缺口盖住了很久。
        run->on_click = [&app]() {
            app.scan_async();
            app.rebuild_ui();
        };

        auto* clear = act_row->emplace<Button>();
        clear->variant = Button::Variant::Text;
        clear->label = L"清空";
        clear->block = false;
        clear->width_hint = 96.0f;
        clear->enabled = !app.busy;
        clear->on_click = [&app]() {
            app.clear_scans();
            app.rebuild_ui();
        };
    }

    // ---------------- 图片列表 ----------------
    if (!app.scan_items.empty()) {
        auto* list_title = left->emplace<SectionTitle>();
        list_title->title = L"这一批";
        list_title->trailing = L"点一行查看 / 修改那一张";

        auto* list_card = left->emplace<Card>();
        list_card->padding = Insets::all(12.0f);
        list_card->gap = 8.0f;

        for (size_t i = 0; i < app.scan_items.size(); ++i) {
            const ScanItem& it = app.scan_items[i];
            auto* row = list_card->emplace<ScanRow>();
            row->index = static_cast<int>(i);
            row->label = it.label;
            row->dims = it.dims;
            row->state = static_cast<int>(it.state);
            row->selected = (static_cast<int>(i) == app.scan_selected);
            row->applied = it.applied;

            if (it.applied) {
                row->summary = L"已记入账本";
            } else if (it.draft.ready) {
                row->summary = ui::to_wide(Money::from_minor(it.draft.amount_minor).to_grouped_string()) +
                               (it.draft.category_name.empty()
                                    ? std::wstring(L"")
                                    : L"　" + ui::to_wide(it.draft.category_name));
            } else if (it.state == ScanItem::State::Running) {
                row->summary = L"识别中…";
            } else if (it.state == ScanItem::State::Failed) {
                row->summary = L"识别失败：" + ui::to_wide(it.fail_reason);
            } else {
                row->summary = L"待识别";
            }

            row->on_open = [&app, i]() {
                app.scan_selected = static_cast<int>(i);
                app.rebuild_ui();
            };
            row->on_drop = [&app, i]() {
                app.drop_scan(static_cast<int>(i));
                app.rebuild_ui();
            };
        }
    }

    // ---------------- 当前选中那一张 ----------------
    ScanItem* item = app.cur_item();
    if (item != nullptr) {
        auto* det_title = right->emplace<SectionTitle>();
        det_title->title = L"第 " + std::to_wstring(app.scan_selected + 1) + L" 张 · " + item->label;
        det_title->trailing = item->applied ? L"已入账"
                                            : (item->draft.ready ? L"核对后入账" : L"还没识别");

        auto* card = right->emplace<Card>();
        card->padding = Insets::all(20.0f);
        card->gap = 12.0f;

        auto* preview = card->emplace<ImagePreview>();
        preview->path = item->path;
        preview->height = 140.0f;
        preview->placeholder = L"（这一张的原图预览）";

        if (!item->draft.ready && !item->fail_reason.empty()) {
            auto* err = card->emplace<ErrorBanner>();
            err->text = ui::to_wide(item->fail_reason);
        }

        if (item->draft.ready) {
            // ---- 金额（可改：模型看错数字是常事，不该逼用户放弃整张） ----
            auto* amount = card->emplace<TextField>();
            amount->prefix = L"¥";
            amount->numeric = true;
            amount->style = TypeStyle::DisplaySmall;
            amount->placeholder = L"0.00";
            amount->value = amount_edit_text(item->draft.amount_minor);
            amount->supporting = L"识别出的金额，不对就直接改";
            amount->on_change = [&app, amount]() {
                auto m = Money::parse(ui::to_utf8(amount->value));
                if (m.is_ok()) app.cur_draft().amount_minor = m.value().minor_units();
            };

            // ---- 方向 ----
            auto* dir = card->emplace<SegmentedTabs>();
            dir->items = {L"支出", L"收入"};
            dir->selected = item->draft.direction;
            dir->on_change = [&app](int idx) {
                if (app.cur_draft().direction == idx) return;
                app.cur_draft().direction = idx;
                app.cur_draft().category_id.clear();   // 方向变了，原分类多半不适用
                app.scan_cats_expanded = false;
                app.rebuild_ui();
            };

            // ---- 分类（可改：这是识别最容易出错的地方） ----
            const Direction want =
                (item->draft.direction == 1) ? Direction::Income : Direction::Expense;
            std::vector<const Category*> cats;
            for (const auto& c : app.categories) {
                if (c.direction == want && c.is_active) cats.push_back(&c);
            }

            auto* cat_title = card->emplace<Label>();
            cat_title->text = L"分类（点一下改）";
            cat_title->style = TypeStyle::LabelLarge;

            constexpr size_t kScanCatsCollapsed = 12;
            const bool many = cats.size() > kScanCatsCollapsed;
            const size_t shown = (many && !app.scan_cats_expanded) ? kScanCatsCollapsed : cats.size();

            auto* grid = card->emplace<Grid>();
            grid->gap_x = 8.0f;
            grid->gap_y = 8.0f;
            grid->cell_height = 76.0f;
            // 0 = 按最小格宽自动算列数（与记账页一致）
            grid->columns = 0;
            grid->min_cell_width = 132.0f;

            if (item->draft.category_id.empty() && !item->draft.category_name.empty()) {
                item->draft.category_id = app.category_id_of_name(item->draft.category_name);
            }

            for (size_t i = 0; i < shown && i < cats.size(); ++i) {
                const Category* c = cats[i];
                auto* chip = grid->emplace<Chip>();
                chip->text = ui::to_wide(c->name);
                chip->dot = Color::from_hex(c->color_hex, th.palette.outline);
                chip->selected = (c->id == item->draft.category_id);
                const std::string id = c->id;
                const std::string nm = c->name;
                chip->on_click = [&app, id, nm]() {
                    app.cur_draft().category_id = id;
                    app.cur_draft().category_name = nm;
                    app.rebuild_ui();
                };
            }

            if (many) {
                auto* more = card->emplace<Button>();
                more->variant = Button::Variant::Text;
                more->block = false;
                more->width_hint = 200.0f;
                more->label = app.scan_cats_expanded
                                  ? L"收起"
                                  : (L"展开全部 " + std::to_wstring(cats.size()) + L" 个分类");
                more->on_click = [&app]() {
                    app.scan_cats_expanded = !app.scan_cats_expanded;
                    app.rebuild_ui();
                };
            }

            // ---- 其它识别字段（只读展示） ----
            auto* kv_date = card->emplace<KeyValueRow>();
            kv_date->key = L"日期";
            kv_date->value = ui::to_wide(item->draft.date_iso.empty() ? Date::today().to_string()
                                                                      : item->draft.date_iso);

            if (!item->draft.merchant.empty()) {
                auto* kv = card->emplace<KeyValueRow>();
                kv->key = L"商户";
                kv->value = ui::to_wide(item->draft.merchant);
            }
            if (!item->draft.note.empty()) {
                auto* kv = card->emplace<KeyValueRow>();
                kv->key = L"说明";
                kv->value = ui::to_wide(item->draft.note);
            }

            auto* conf = card->emplace<ConfidenceBar>();
            conf->value = static_cast<float>(item->draft.confidence);

            if (item->draft.confidence > 0.0 && item->draft.confidence < 0.6) {
                auto* low = card->emplace<InfoNote>();
                low->warn = true;
                low->text = L"模型对这个金额不太有把握，请对照上面的原图核对一遍再入账。";
            }
        }

        auto* actions = card->emplace<HBox>();
        actions->gap = 12.0f;
        actions->align = HBox::Align::Stretch;

        auto* ok = actions->emplace<Button>();
        ok->flex = 2.0f;
        ok->variant = Button::Variant::Filled;
        ok->label = item->applied ? L"已记入账本"
                                  : (item->draft.ready ? L"确认入账" : L"等待识别");
        ok->enabled = item->draft.ready && !item->applied;
        ok->on_click = [&app]() {
            app.confirm_scan();
            app.rebuild_ui();
        };

        auto* drop = actions->emplace<Button>();
        drop->flex = 1.0f;
        drop->variant = Button::Variant::Text;
        drop->label = item->applied ? L"移除这张" : L"放弃这张";
        drop->on_click = [&app]() {
            app.drop_scan(app.scan_selected);
            app.rebuild_ui();
        };
    }

    // ---------------- 做法说明（透明度） ----------------
    auto* how = col.emplace<Card>();
    how->padding = Insets::all(18.0f);
    how->gap = 8.0f;

    auto* how_title = how->emplace<Label>();
    how_title->text = L"识别是怎么做的";
    how_title->style = TypeStyle::TitleSmall;

    auto* how_body = how->emplace<Label>();
    how_body->wrap = true;
    how_body->style = TypeStyle::BodySmall;
    how_body->color = th.palette.on_surface_variant;
    how_body->text =
        L"1. 每张图片在本机缩放到最长边 1600 像素，并重新编码为 JPEG（否则手机截图的体积会"
        L"超过多数接口的上限）。多张时**顺序逐张**发，不并发 —— 并发容易撞限流，"
        L"而且顺序跑才有稳定的进度可看。\n"
        L"2. 缩放后的图片以 data URL 形式发给你在「设置」里配置的那个模型通道 —— "
        L"走云端就等于把这些截图都发到了那个服务商，走本机 llama-server 则不出本机。"
        L"这一点请按你的接受程度选择通道。\n"
        L"3. 模型被要求只回一个 JSON；我们会解析它，并把分类名对齐到你账号里的实际分类。\n"
        L"4. 读不出金额时直接报「未识别」，不会用别的数字顶替 —— "
        L"宁可让你手填，也不让一个猜出来的金额悄悄进账本。\n"
        L"5. 识别结果只是草稿：金额、分类都能改，点「确认入账」才会写进账本。";

    auto* how_channel = how->emplace<Label>();
    how_channel->wrap = true;
    how_channel->style = TypeStyle::BodySmall;
    how_channel->color = th.palette.on_surface_variant;
    how_channel->text = ui::to_wide(app.channel_notes);
}

}  // namespace penhu::native
