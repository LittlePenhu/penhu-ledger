// =============================================================================
//  native/pages/entry.cpp
//  记账页：金额 + 方向 + 分类网格 + 日期 + 备注 + 最近记录。
// =============================================================================

#include <algorithm>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "penhu/domain/money.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {

using namespace ui;

/// 日期行：◀ 日期 ▶，点两端按天调整。
/// 为什么不做日历弹层：一次记账改日期通常只是「昨天忘了记」这种一两天的位移，
/// 左右箭头一次点击就够；日历控件要占掉大半个屏，反而更慢。
class DateRow : public Widget {
public:
    std::wstring date_text;
    std::wstring hint;
    std::function<void(int)> on_shift;   // -1 / +1

    float preferred_height(float, Renderer&) override { return 52.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        const float btn = 44.0f;
        const Rect left{bounds_.x, bounds_.y + (bounds_.h - btn) * 0.5f, btn, btn};
        const Rect right{bounds_.right() - btn, left.y, btn, btn};

        for (int side = 0; side < 2; ++side) {
            const Rect box = (side == 0) ? left : right;
            const bool hot = (pressed_side_ == (side == 0 ? -1 : 1)) ||
                             (hover_side_ == (side == 0 ? -1 : 1));
            if (hot) r.fill_circle(box.center_x(), box.center_y(), btn * 0.5f,
                                   th.palette.on_surface.alpha_of(Theme::kStateHover));
            r.text_line(side == 0 ? L"◀" : L"▶", box, TypeStyle::BodyMedium,
                        th.palette.on_surface_variant, TextAlign::Center, false);
        }

        r.text_line(date_text, {left.right() + 8.0f, bounds_.y, bounds_.w - btn * 2 - 16.0f, bounds_.h},
                    TypeStyle::TitleSmall, th.palette.on_surface, TextAlign::Center, true);
        if (!hint.empty()) {
            r.text_line(hint, {right.x - 70.0f, bounds_.y, 60.0f, bounds_.h}, TypeStyle::BodySmall,
                        th.palette.primary, TextAlign::Right, false);
        }
    }

    bool on_pointer_move(const Point& p, bool inside) override {
        hover_side_ = inside ? side_at(p) : 0;
        return true;
    }
    bool on_pointer_down(const Point& p) override {
        pressed_side_ = side_at(p);
        return true;
    }
    bool on_pointer_up(const Point& p) override {
        const int side = (side_at(p) == pressed_side_) ? pressed_side_ : 0;
        pressed_side_ = 0;
        if (side != 0 && on_shift) on_shift(side);
        return true;
    }

private:
    int side_at(const Point& p) const {
        if (!bounds_.contains(p)) return 0;
        const float btn = 44.0f;
        if (p.x <= bounds_.x + btn) return -1;
        if (p.x >= bounds_.right() - btn) return 1;
        return 0;
    }
    int hover_side_{0};
    int pressed_side_{0};
};

std::wstring weekday_hint(const std::string& iso) {
    auto d = Date::parse(iso);
    if (d.is_err()) return {};
    const Date& date = d.value();
    const std::string today = Date::today().to_string();
    if (iso == today) return L"今天";
    if (iso == Date::today().add_days(-1).to_string()) return L"昨天";
    return ui::to_wide(date.weekday_zh());
}

}  // namespace

void build_entry_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    // 原来这里有个 64dp 的页头。撤掉的理由：顶栏已经写着「记账」，
    // 再来一行「记一笔」是同一个意思说两遍。
    // 但「正在编辑哪一笔」和统计口径是真信息，下沉成一行小字。
    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = page_padding(app);
    col.gap = 16.0f;
    col.max_width = app.content_max_width();

    {
        std::wstring note;
        if (!app.editing_record_id.empty()) note += L"正在编辑一笔已有记录　";
        if (!app.data_note.empty()) note += ui::to_wide(app.data_note);
        info_line(col, th, note);
    }


    // 宽屏双栏：左边「记一笔」，右边「最近记录」。
    // 记账和信息核对是两件事，窄屏时上下堆着（要滚），宽屏并排就能
    // 一边看列表一边改金额，不用来回滚。
    //
    // 窄屏时两个指针指向同一列 —— 内容按代码顺序依次堆叠，
    // 正好和原来单栏版一模一样，不需要写两套。
    VBox* left = &col;
    VBox* right = &col;
    if (app.wide()) {
        auto* row = col.emplace<HBox>();
        row->gap = 20.0f;
        row->align = HBox::Align::Start;   // 两列各自决定高度，不互相拉平
        auto* l = row->emplace<VBox>();
        l->flex = 1.0f;
        l->gap = 16.0f;
        auto* r = row->emplace<VBox>();
        r->flex = 1.0f;
        r->gap = 16.0f;
        left = l;
        right = r;
    }

    // ---------------- 金额 + 方向 ----------------
    auto* amount_card = left->emplace<Card>();
    amount_card->padding = Insets::all(20.0f);
    amount_card->gap = 14.0f;

    auto* dir = amount_card->emplace<SegmentedTabs>();
    dir->items = {L"支出", L"收入"};
    dir->selected = app.entry_direction;
    dir->on_change = [&app](int idx) {
        if (app.entry_direction == idx) return;
        app.entry_direction = idx;
        app.entry_category_id.clear();   // 方向换了，原来的分类多半不适用
        app.rebuild_ui();
    };

    auto* amount = amount_card->emplace<TextField>();
    amount->prefix = L"¥";
    amount->numeric = true;
    amount->style = TypeStyle::DisplaySmall;
    amount->placeholder = L"0.00";
    amount->value = app.entry_amount;
    amount->on_change = [&app, amount]() { app.entry_amount = amount->value; };
    amount->on_submit = [&app]() { app.save_entry(); app.rebuild_ui(); };

    // ---------------- 分类 ----------------
    const Direction want = (app.entry_direction == 0) ? Direction::Expense : Direction::Income;
    std::vector<const Category*> cats;
    for (const auto& c : app.categories) {
        if (c.direction == want && c.is_active) cats.push_back(&c);
    }

    auto* cat_card = left->emplace<Card>();
    cat_card->padding = Insets::all(16.0f);
    cat_card->gap = 12.0f;

    auto* cat_title = cat_card->emplace<Label>();
    cat_title->text = L"分类";
    cat_title->style = TypeStyle::TitleSmall;

    auto* grid = cat_card->emplace<Grid>();
    // 0 = 按最小格宽自动算列数。预置分类有 40 多个支出项，
    // 固定 4 列会拉出十几行，记账时得滚半天才找到目标分类。
    grid->columns = 0;
    grid->min_cell_width = 126.0f;
    grid->gap_x = 8.0f;
    grid->gap_y = 8.0f;
    grid->cell_height = 72.0f;

    if (cats.empty()) {
        auto* empty = grid->emplace<Label>();
        empty->text = L"这个方向还没有分类，去「设置 → 分类管理」启用一个。";
        empty->style = TypeStyle::BodySmall;
        empty->color = th.palette.on_surface_variant;
    } else {
        if (app.entry_category_id.empty()) {
            app.entry_category_id = cats.front()->id;
        }

        // 默认只铺前两行，其余折起来。展开条件有两个：
        // 用户主动点了「更多分类」，或者当前选中的分类本来就落在折叠区之外
        // （编辑一笔「税费」时如果看不见它被选中，会以为没选上）。
        constexpr size_t kFoldLimit = 12;
        size_t selected_index = 0;
        for (size_t i = 0; i < cats.size(); ++i) {
            if (cats[i]->id == app.entry_category_id) {
                selected_index = i;
                break;
            }
        }
        const bool expand = app.entry_show_all_categories || selected_index >= kFoldLimit ||
                            cats.size() <= kFoldLimit;
        const size_t shown = expand ? cats.size() : kFoldLimit;

        for (size_t i = 0; i < shown; ++i) {
            const Category* c = cats[i];
            auto* chip = grid->emplace<Chip>();
            chip->text = ui::to_wide(c->name);
            chip->dot = Color::from_hex(c->color_hex, th.palette.outline);
            chip->selected = (c->id == app.entry_category_id);
            const std::string id = c->id;
            chip->on_click = [&app, id]() {
                app.entry_category_id = id;
                app.rebuild_ui();
            };
        }

        if (cats.size() > kFoldLimit) {
            auto* more = cat_card->emplace<Button>();
            more->variant = Button::Variant::Text;
            more->block = true;
            more->min_height = 44.0f;
            more->label = app.entry_show_all_categories
                              ? L"收起分类"
                              : (L"更多分类（还有 " +
                                 std::to_wstring(cats.size() - kFoldLimit) + L" 个）");
            more->glyph = app.entry_show_all_categories ? L"▲" : L"▼";
            more->on_click = [&app]() {
                app.entry_show_all_categories = !app.entry_show_all_categories;
                app.rebuild_ui();
            };
        }
    }

    // ---------------- 日期 + 备注 ----------------
    auto* misc = left->emplace<Card>();
    misc->padding = Insets::all(16.0f);
    misc->gap = 10.0f;

    auto* date_row = misc->emplace<DateRow>();
    date_row->date_text = ui::to_wide(app.entry_date_iso);
    date_row->hint = weekday_hint(app.entry_date_iso);
    date_row->on_shift = [&app](int delta) {
        auto d = Date::parse(app.entry_date_iso);
        const Date base = d.is_ok() ? d.value() : Date::today();
        app.entry_date_iso = base.add_days(delta).to_string();
        app.rebuild_ui();
    };

    auto* note = misc->emplace<TextField>();
    note->placeholder = L"备注（可空，例如「兰州拉面」）";
    note->value = app.entry_note;
    note->style = TypeStyle::BodyLarge;
    note->on_change = [&app, note]() { app.entry_note = note->value; };

    // ---------------- 错误 + 保存 ----------------
    if (!app.entry_error.empty()) {
        auto* err = left->emplace<ErrorBanner>();
        err->text = ui::to_wide(app.entry_error);
    }

    auto* actions = left->emplace<HBox>();
    actions->gap = 12.0f;
    actions->align = HBox::Align::Stretch;

    auto* save = actions->emplace<Button>();
    save->flex = 2.0f;
    save->variant = Button::Variant::Filled;
    save->label = app.editing_record_id.empty() ? L"记入账本" : L"更新这笔";
    save->on_click = [&app]() {
        app.save_entry();
        app.rebuild_ui();
    };

    if (!app.editing_record_id.empty()) {
        auto* cancel = actions->emplace<Button>();
        cancel->flex = 1.0f;
        cancel->variant = Button::Variant::Text;
        cancel->label = L"取消编辑";
        cancel->on_click = [&app]() {
            app.cancel_edit();
            app.rebuild_ui();
        };
        auto* del = actions->emplace<Button>();
        del->flex = 1.0f;
        del->variant = Button::Variant::Danger;
        del->label = L"删除";
        del->on_click = [&app]() {
            const std::string id = app.editing_record_id;
            app.cancel_edit();
            app.delete_record(id);
            app.rebuild_ui();
        };
    }

    // ---------------- 最近记录 ----------------
    auto* head = right->emplace<Label>();
    head->text = L"最近记录";
    head->style = TypeStyle::TitleSmall;

    auto* list_card = right->emplace<Card>();
    list_card->padding = {0.0f, 4.0f, 0.0f, 4.0f};
    list_card->gap = 0.0f;

    if (app.records.empty()) {
        auto* empty = list_card->emplace<Label>();
        empty->text = L"还没有记录。填个金额、点一下分类、按「记入账本」就完成了。";
        empty->style = TypeStyle::BodySmall;
        empty->color = th.palette.on_surface_variant;
        empty->padding = Insets::all(16.0f);
    } else {
        int shown = 0;
        for (const Record& rec : app.records) {
            if (shown >= 8) break;
            ++shown;

            auto* row = list_card->emplace<ListRow>();
            const std::string cat_name = app.category_name_of(rec.category_id);
            row->title = ui::to_wide(rec.note.empty() ? cat_name : rec.note);
            row->subtitle = ui::to_wide(rec.date.to_string() + " · " +
                                        (cat_name.empty() ? "未分类" : cat_name));
            row->trailing = ui::to_wide(rec.amount.to_grouped_string());
            row->trailing_color = (rec.direction == Direction::Expense) ? th.palette.expense
                                                                       : th.palette.income;
            for (const auto& c : app.categories) {
                if (c.id == rec.category_id) {
                    row->dot = Color::from_hex(c.color_hex, th.palette.outline);
                    break;
                }
            }
            row->selected = (rec.id == app.editing_record_id);
            const std::string id = rec.id;
            row->on_click = [&app, id]() {
                app.begin_edit(id);
                app.rebuild_ui();
            };
        }
    }

    if (app.records.size() > 8) {
        // 这一行原来是句死文案（「只显示最近 8 条」），看完只能干瞪眼。
        // 改成按钮直达「明细」页 —— 想看第 9 条的路就在第 8 条下面。
        auto* more = right->emplace<Button>();
        more->variant = Button::Variant::Text;
        more->block = true;
        more->min_height = 44.0f;
        more->glyph = L"▤";
        more->label = L"查看全部 " + std::to_wstring(app.records.size()) + L" 条记录";
        more->on_click = [&app]() { app.go(Screen::Records); };
    }
}

}  // namespace penhu::native
