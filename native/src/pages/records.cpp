// =============================================================================
//  native/pages/records.cpp
//  全部记录页：筛选（方向 / 分类 / 备注关键词）+ 分页浏览整个账本。
//
//  为什么是内存过滤而不是走 list_records 的分页查询：
//  refresh() 本来就把全量记录读进了 App::records（统计页要全量算），
//  几千条在本地过滤是微秒级的事；而组件树这边只要每页铺 50 行就够 ——
//  真正会卡的是「一次建几千个 ListRow」，不是「扫几千条数据」。
//  记录到十万级时再换成 RecordQuery.offset 下推（和 snapshot 的取舍一致）。
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <vector>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "penhu/domain/money.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
using namespace ui;

/// 一页铺多少行。50 行 × 60dp ≈ 3000dp 的组件量，重建一次毫秒级。
constexpr int kPageSize = 50;

/// 分类网格默认铺几格（和记账页一致：40 多个分类全铺开会把列表推出屏幕）
constexpr size_t kFoldLimit = 12;

std::wstring lower_copy(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

/// 「YYYY-MM-DD」的字符串序就是时间序，不用真解析日期。
bool newer_first(const Record& a, const Record& b) {
    const std::string da = a.date.to_string();
    const std::string db = b.date.to_string();
    if (da != db) return da > db;
    return a.created_at > b.created_at;   // 同日新记的在上
}

}  // namespace

void build_records_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = page_padding(app);
    col.gap = 16.0f;
    col.max_width = app.content_max_width();

    // ---------------- 过滤 ----------------
    std::vector<const Record*> rows;
    rows.reserve(app.records.size());
    const std::wstring kw = lower_copy(app.rec_keyword);
    int64_t sum_expense = 0;
    int64_t sum_income = 0;
    for (const Record& rec : app.records) {
        if (app.rec_filter_dir == 1 && rec.direction != Direction::Expense) continue;
        if (app.rec_filter_dir == 2 && rec.direction != Direction::Income) continue;
        if (!app.rec_filter_cat.empty() && rec.category_id != app.rec_filter_cat) continue;
        if (!kw.empty() && lower_copy(ui::to_wide(rec.note)).find(kw) == std::wstring::npos) continue;
        rows.push_back(&rec);
        if (rec.direction == Direction::Expense) sum_expense += rec.amount.minor_units();
        else sum_income += rec.amount.minor_units();
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Record* a, const Record* b) { return newer_first(*a, *b); });

    // ---------------- 口径行 ----------------
    {
        std::wstring note = L"共 " + std::to_wstring(app.records.size()) + L" 条记录";
        if (rows.size() != app.records.size()) {
            note += L"，当前筛选出 " + std::to_wstring(rows.size()) + L" 条";
        }
        info_line(col, th, note);
    }

    // ---------------- 筛选控件 ----------------
    auto* filter = col.emplace<Card>();
    filter->padding = Insets::all(16.0f);
    filter->gap = 12.0f;

    auto* dir = filter->emplace<SegmentedTabs>();
    dir->items = {L"全部", L"支出", L"收入"};
    dir->selected = app.rec_filter_dir;
    dir->on_change = [&app](int idx) {
        if (app.rec_filter_dir == idx) return;
        app.rec_filter_dir = idx;
        app.rec_filter_cat.clear();   // 方向换了，原分类筛选多半落空
        app.rec_page = 0;
        app.rebuild_ui();
    };

    // 关键词：输入框只存值（不能重建树，否则光标/输入法丢），回车或点按钮才过滤
    auto* kw_row = filter->emplace<HBox>();
    kw_row->gap = 8.0f;
    kw_row->align = HBox::Align::Stretch;

    auto* kw_box = kw_row->emplace<TextField>();
    kw_box->flex = 1.0f;
    kw_box->placeholder = L"按备注搜索（例如「拉面」），回车生效";
    kw_box->value = app.rec_keyword;
    kw_box->on_change = [&app, kw_box]() { app.rec_keyword = kw_box->value; };
    kw_box->on_submit = [&app]() {
        app.rec_page = 0;
        app.rebuild_ui();
    };

    auto* kw_btn = kw_row->emplace<Button>();
    kw_btn->variant = Button::Variant::Tonal;
    kw_btn->label = L"筛选";
    kw_btn->on_click = [&app]() {
        app.rec_page = 0;
        app.rebuild_ui();
    };

    // 分类筛选：只列「当前方向下有记录的分类」，不是全部预置分类 ——
    // 41 个预置支出分类里通常只有十来个真被用过，把没记录的也铺出来
    // 只是让用户在一堆「筛了也是 0 条」的格子里找目标。
    struct CatStat {
        std::string id;
        std::string name;
        size_t      count{0};
    };
    std::vector<CatStat> used;
    for (const Record& rec : app.records) {
        if (app.rec_filter_dir == 1 && rec.direction != Direction::Expense) continue;
        if (app.rec_filter_dir == 2 && rec.direction != Direction::Income) continue;
        bool found = false;
        for (auto& cs : used) {
            if (cs.id == rec.category_id) { ++cs.count; found = true; break; }
        }
        if (!found) {
            used.push_back({rec.category_id, app.category_name_of(rec.category_id), 1});
        }
    }
    std::stable_sort(used.begin(), used.end(),
                     [](const CatStat& a, const CatStat& b) { return a.count > b.count; });

    if (!used.empty()) {
        auto* grid = filter->emplace<Grid>();
        grid->columns = 0;
        grid->min_cell_width = 126.0f;
        grid->gap_x = 8.0f;
        grid->gap_y = 8.0f;
        grid->cell_height = 72.0f;

        {
            auto* all = grid->emplace<Chip>();
            all->text = L"全部分类";
            all->selected = app.rec_filter_cat.empty();
            all->on_click = [&app]() {
                if (app.rec_filter_cat.empty()) return;
                app.rec_filter_cat.clear();
                app.rec_page = 0;
                app.rebuild_ui();
            };
        }

        size_t selected_index = 0;   // 0 = 「全部分类」那一格
        for (size_t i = 0; i < used.size(); ++i) {
            if (used[i].id == app.rec_filter_cat) {
                selected_index = i + 1;
                break;
            }
        }
        // 和记账页同一个规则：选中项在折叠区外时自动展开，避免「选了却看不见」
        const bool expand = app.rec_cats_expanded || selected_index > kFoldLimit ||
                            used.size() + 1 <= kFoldLimit;
        const size_t shown = expand ? used.size() : (kFoldLimit - 1);

        for (size_t i = 0; i < shown; ++i) {
            auto* chip = grid->emplace<Chip>();
            chip->text = ui::to_wide(used[i].name.empty() ? "未分类" : used[i].name);
            for (const auto& c : app.categories) {
                if (c.id == used[i].id) {
                    chip->dot = Color::from_hex(c.color_hex, th.palette.outline);
                    break;
                }
            }
            chip->selected = (used[i].id == app.rec_filter_cat);
            const std::string id = used[i].id;
            chip->on_click = [&app, id]() {
                app.rec_filter_cat = id;
                app.rec_page = 0;
                app.rebuild_ui();
            };
        }

        if (used.size() + 1 > kFoldLimit) {
            auto* more = filter->emplace<Button>();
            more->variant = Button::Variant::Text;
            more->block = true;
            more->min_height = 44.0f;
            more->label = app.rec_cats_expanded ? L"收起分类" : L"更多分类";
            more->glyph = app.rec_cats_expanded ? L"▲" : L"▼";
            more->on_click = [&app]() {
                app.rec_cats_expanded = !app.rec_cats_expanded;
                app.rebuild_ui();
            };
        }
    }

    // ---------------- 列表 ----------------
    auto* head = col.emplace<SectionTitle>();
    head->title = L"记录明细";
    head->trailing = L"支出 " + ui::to_wide(Money::from_minor(sum_expense).to_grouped_string()) +
                     L"　收入 " + ui::to_wide(Money::from_minor(sum_income).to_grouped_string());

    auto* list_card = col.emplace<Card>();
    list_card->padding = {0.0f, 4.0f, 0.0f, 4.0f};
    list_card->gap = 0.0f;

    const int total = static_cast<int>(rows.size());
    const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    if (app.rec_page >= pages) app.rec_page = pages - 1;   // 删记录后可能越界，就地夹住
    if (app.rec_page < 0) app.rec_page = 0;
    const int begin = app.rec_page * kPageSize;
    const int end = std::min(total, begin + kPageSize);

    if (total == 0) {
        auto* empty = list_card->emplace<Label>();
        empty->text = app.records.empty()
                          ? L"还没有记录。去「记账」页记一笔就有了。"
                          : L"没有符合筛选条件的记录。换个方向/分类/关键词再试试。";
        empty->style = TypeStyle::BodySmall;
        empty->color = th.palette.on_surface_variant;
        empty->padding = Insets::all(16.0f);
    } else {
        for (int i = begin; i < end; ++i) {
            const Record& rec = *rows[static_cast<size_t>(i)];
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
                // 点一行 = 转去记账页编辑这笔（复用那里的更新/删除按钮）
                app.begin_edit(id);
                app.rebuild_ui();
            };
        }
    }

    // ---------------- 翻页 ----------------
    if (total > 0) {
        auto* pager = col.emplace<HBox>();
        pager->gap = 12.0f;
        pager->align = HBox::Align::Center;

        auto* prev = pager->emplace<Button>();
        prev->variant = Button::Variant::Outlined;
        prev->label = L"上一页";
        prev->on_click = [&app]() {
            if (app.rec_page > 0) {
                --app.rec_page;
                app.rebuild_ui();
            }
        };

        auto* pos = pager->emplace<Label>();
        pos->text = L"第 " + std::to_wstring(app.rec_page + 1) + L" / " +
                    std::to_wstring(pages) + L" 页（" + std::to_wstring(total) + L" 条）";
        pos->style = TypeStyle::BodySmall;
        pos->color = th.palette.on_surface_variant;
        pos->wrap = false;

        auto* next = pager->emplace<Button>();
        next->variant = Button::Variant::Outlined;
        next->label = L"下一页";
        next->on_click = [&app, pages]() {
            if (app.rec_page + 1 < pages) {
                ++app.rec_page;
                app.rebuild_ui();
            }
        };
    }
}

}  // namespace penhu::native
