// =============================================================================
//  native/pages/settings.cpp
//  设置页：模型通道 + 截图识别要求 + 安全信息 + 分类概况 + 关于。
//
//  安全信息这一块不是装饰：它把「密钥在哪、数据在哪、什么时候会出本机」
//  摊开写清楚。用户要判断能不能把账本交给这个程序，只能靠这些事实。
// =============================================================================

#include <algorithm>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "penhu/llm/settings.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
using namespace ui;

/// 「标签 + 值」两列一行（与截图页的 KeyValueRow 同构，但那条在页内是私有实现，
/// 这里用不同名字以免两处各自演化时互相牵扯）
class SettingRow : public Widget {
public:
    std::wstring key;
    std::wstring value;

    float preferred_height(float width, Renderer& r) override {
        const float vw = std::max(60.0f, width - kKeyW - kGapW);
        return std::max(34.0f, r.measure(value, TypeStyle::BodySmall, vw, true).h + 14.0f);
    }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        // key 用正文字号：它是这一行的「标签」，比说明文字更该显眼。
        // 之前 key 和 value 都是 12px，一列下来分不出主次。
        r.text_line(key, {bounds_.x, bounds_.y, kKeyW - 12.0f, bounds_.h},
                    TypeStyle::BodyMedium, th.palette.on_surface_variant, TextAlign::Left, false);
        r.text(value, {bounds_.x + kKeyW, bounds_.y + 6.0f, bounds_.w - kKeyW, bounds_.h - 12.0f},
               TypeStyle::BodySmall, th.palette.on_surface, TextAlign::Left, VAlign::Middle,
               true, true);
    }

private:
    /// key 列宽 + 与 value 的间距。key 里最长的是「数据目录」这类四字词，
    /// 118 足够；之前 100 连「密钥派生」都要挤。
    static constexpr float kKeyW = 124.0f;
    static constexpr float kGapW = 16.0f;
};

/// 带标题的卡片小节
Card* section(VBox& col, const std::wstring& title, const std::wstring& trailing = {}) {
    auto* head = col.emplace<SectionTitle>();
    head->title = title;
    head->trailing = trailing;
    auto* card = col.emplace<Card>();
    card->padding = Insets::all(18.0f);
    card->gap = 10.0f;
    return card;
}

}  // namespace

void build_settings_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    const Theme th = app.dark ? Theme::dark() : Theme::light();

    // 页头撤掉。「退出登录」搬到顶栏（每一页都能用，不用特意跑回设置页），
    // 副标题那句「模型通道、识别要求、安全信息」只是目录式描述，没有信息量。
    auto* scroll = host.emplace<ScrollView>();
    scroll->flex = 1.0f;

    VBox& col = scroll->content;
    col.padding = {20.0f, 12.0f, 20.0f, 24.0f};
    col.gap = 16.0f;
    col.max_width = 900.0f;

    // ---------------- 模型通道 ----------------
    auto* ch = section(col, L"模型通道", L"决定报告由谁写");

    auto* mode = ch->emplace<SegmentedTabs>();
    mode->items = {L"自动（云端优先）", L"只用云端", L"只用本地"};
    mode->selected = app.set_channel;
    mode->on_change = [&app](int idx) { app.set_channel = idx; };

    auto* base = ch->emplace<TextField>();
    base->placeholder = L"https://api.example.com/v1";
    base->value = app.set_base_url;
    base->style = TypeStyle::BodyMedium;
    base->supporting = L"OpenAI 兼容的 base_url，不要带 /chat/completions";
    base->on_change = [&app, base]() { app.set_base_url = base->value; };

    auto* model = ch->emplace<TextField>();
    model->placeholder = L"模型名，例如 gpt-4o / qwen-vl-max";
    model->value = app.set_model;
    model->style = TypeStyle::BodyMedium;
    model->supporting = L"要同时用于报告和截图识别的话，这里填一个支持图片输入的模型";
    model->on_change = [&app, model]() { app.set_model = model->value; };

    auto* key = ch->emplace<TextField>();
    key->placeholder = L"API Key";
    key->value = app.set_api_key;
    key->password = true;
    key->style = TypeStyle::BodyMedium;
    key->supporting = app.api_key_present
                          ? L"已保存（用该账号的账本密钥再做一层 AEAD 后落盘）。留空表示不修改。"
                          : L"尚未设置。填好后点「保存设置」。";
    key->on_change = [&app, key]() { app.set_api_key = key->value; };

    auto* save_row = ch->emplace<HBox>();
    save_row->gap = 12.0f;
    save_row->align = HBox::Align::Stretch;

    auto* save = save_row->emplace<Button>();
    save->flex = 2.0f;
    save->variant = Button::Variant::Filled;
    save->label = L"保存设置";
    save->on_click = [&app]() {
        app.save_llm_settings();
        app.rebuild_ui();
    };

    auto* reload = save_row->emplace<Button>();
    reload->flex = 1.0f;
    reload->variant = Button::Variant::Outlined;
    reload->label = L"重新读取";
    reload->on_click = [&app]() {
        app.load_llm_settings();
        app.rebuild_ui();
    };

    auto* notes = ch->emplace<InfoNote>();
    notes->text = ui::to_wide(app.channel_notes.empty()
                                  ? std::string("通道配置解析结果会显示在这里。")
                                  : app.channel_notes);

    // ---------------- 截图目录（批量导入） ----------------
    auto* imp = section(col, L"截图目录", L"批量导入与自动记账");

    auto* dir_row = imp->emplace<HBox>();
    dir_row->gap = 12.0f;
    dir_row->align = HBox::Align::Center;

    auto* dir_col = dir_row->emplace<VBox>();
    dir_col->flex = 1.0f;
    dir_col->gap = 2.0f;

    auto* dir_title = dir_col->emplace<Label>();
    dir_title->text = L"截图保存位置";
    dir_title->style = TypeStyle::BodyMedium;

    auto* dir_path = dir_col->emplace<Label>();
    // 单行 + 省略号：完整路径在这里会折成三行，把这一节撑得很高。
    // 识别页用的是同一个 short_path()，两处显示保持一致。
    dir_path->wrap = false;
    dir_path->ellipsis = true;
    dir_path->style = TypeStyle::BodySmall;
    dir_path->color = app.scan_dir.empty() ? th.palette.warn : th.palette.on_surface_variant;
    dir_path->text = app.scan_dir.empty() ? L"（还没有设置）" : short_path(app.scan_dir);

    auto* pick_dir = dir_row->emplace<Button>();
    pick_dir->variant = Button::Variant::Tonal;
    pick_dir->label = L"选择目录";
    pick_dir->glyph = L"▤";
    pick_dir->block = false;
    pick_dir->width_hint = 132.0f;
    pick_dir->on_click = [&app]() {
        app.pick_scan_dir();
        app.rebuild_ui();
    };

    // ---- 开关 1：自动扫描 ----
    auto* row_auto = imp->emplace<HBox>();
    // 24 而不是 12：说明文字最多两行，右端贴着开关时视觉上糊成一团。
    row_auto->gap = 24.0f;
    row_auto->align = HBox::Align::Center;

    auto* auto_col = row_auto->emplace<VBox>();
    auto_col->flex = 1.0f;
    auto_col->gap = 2.0f;

    auto* auto_t = auto_col->emplace<Label>();
    auto_t->text = L"打开识图页时自动扫描";
    auto_t->style = TypeStyle::BodyMedium;

    auto* auto_d = auto_col->emplace<Label>();
    auto_d->wrap = true;
    auto_d->style = TypeStyle::BodySmall;
    auto_d->color = th.palette.on_surface_variant;
    auto_d->text = L"目录里有新截图就自动识别，识别过的会跳过。";

    auto* sw_auto = row_auto->emplace<Switch>();
    sw_auto->value = app.scan_auto_scan;
    sw_auto->on_change = [&app](bool v) {
        app.scan_auto_scan = v;
        app.save_scan_settings();
    };

    // ---- 开关 2：识别成功后直接入账 ----
    auto* row_apply = imp->emplace<HBox>();
    row_apply->gap = 24.0f;
    row_apply->align = HBox::Align::Center;

    auto* apply_col = row_apply->emplace<VBox>();
    apply_col->flex = 1.0f;
    apply_col->gap = 2.0f;

    auto* apply_t = apply_col->emplace<Label>();
    apply_t->text = L"识别成功后直接记账";
    apply_t->style = TypeStyle::BodyMedium;

    auto* apply_d = apply_col->emplace<Label>();
    apply_d->wrap = true;
    apply_d->style = TypeStyle::BodySmall;
    apply_d->color = th.palette.on_surface_variant;
    apply_d->text = L"识别完直接写进账本，不再逐张确认。金额或日期异常会被拦下。";

    auto* sw_apply = row_apply->emplace<Switch>();
    sw_apply->value = app.scan_auto_apply;
    sw_apply->on_change = [&app](bool v) {
        app.scan_auto_apply = v;
        app.save_scan_settings();
    };

    if (app.scan_auto_apply) {
        auto* warn = imp->emplace<InfoNote>();
        warn->warn = true;
        warn->text = L"模型认错一个数字，那笔账就会静默进账本。金额经常看错的话，"
                     L"建议关掉它改成逐张确认。";
    }

    // ---- 开关 3：入账后把原图移到回收站 ----
    auto* row_clean = imp->emplace<HBox>();
    row_clean->gap = 24.0f;
    row_clean->align = HBox::Align::Center;

    auto* clean_col = row_clean->emplace<VBox>();
    clean_col->flex = 1.0f;
    clean_col->gap = 2.0f;

    auto* clean_t = clean_col->emplace<Label>();
    clean_t->text = L"入账成功后把原图移到回收站";
    clean_t->style = TypeStyle::BodyMedium;

    auto* clean_d = clean_col->emplace<Label>();
    clean_d->wrap = true;
    clean_d->style = TypeStyle::BodySmall;
    clean_d->color = th.palette.on_surface_variant;
    clean_d->text = L"只清理已记账的那张，走回收站，可以还原。";

    auto* sw_clean = row_clean->emplace<Switch>();
    sw_clean->value = app.scan_auto_clean;
    sw_clean->on_change = [&app](bool v) {
        app.scan_auto_clean = v;
        app.save_scan_settings();
    };

    if (app.scan_auto_clean) {
        auto* warn2 = imp->emplace<InfoNote>();
        warn2->warn = true;
        warn2->text = L"每记一笔账，对应截图就移进回收站。想留档就别开它 —— "
                      L"清空回收站之后原图不可恢复。";
    }

    auto* imp_body = imp->emplace<Label>();
    imp_body->wrap = true;
    imp_body->style = TypeStyle::BodySmall;
    imp_body->color = th.palette.on_surface_variant;
    imp_body->text =
        L"把截图放进这个目录，到「截图」页按「扫描目录」。\n"
        L"识别失败的不会被标记为已处理，配好模型后可以重扫。";

    // ---------------- 截图识别的要求 ----------------
    auto* scan = section(col, L"截图识别的前提", L"没有视觉模型就用不了");

    auto* scan_body = scan->emplace<Label>();
    scan_body->wrap = true;
    scan_body->style = TypeStyle::BodySmall;
    scan_body->color = th.palette.on_surface_variant;
    // 三段长小字压成三行短的。这一节的信息量其实就三条，原文把它写成了
    // 一篇说明文 —— 设置页不该让人读论文。
    scan_body->text =
        L"需要一个能看图的模型（多模态）；纯文本模型收到图片只会忽略它。\n"
        L"可用：gpt-4o、qwen-vl-max、glm-4v，或任何 OpenAI 兼容的视觉端点。\n"
        L"本地视觉模型代码里有支持，但界面还没接入口，目前用云端。";

    // ---------------- 安全信息 ----------------
    auto* sec = section(col, L"安全信息", L"这些是事实，不是承诺");

    auto* r1 = sec->emplace<SettingRow>();
    r1->key = L"数据目录";
    r1->value = ui::to_wide(app.data_dir);

    auto* r2 = sec->emplace<SettingRow>();
    r2->key = L"你的账本";
    r2->value = app.ledger_file.empty() ? L"（未登录）" : (L"data\\" + ui::to_wide(app.ledger_file));

    auto* r3 = sec->emplace<SettingRow>();
    r3->key = L"密钥派生";
    r3->value = L"Argon2id（ops=3，256 MiB，每账号独立 salt）";

    auto* r4 = sec->emplace<SettingRow>();
    r4->key = L"账本加密";
    r4->value = L"SQLCipher 整库加密，密钥只在内存；退出即需重登（刻意取舍）";

    auto* r5 = sec->emplace<SettingRow>();
    r5->key = L"API Key";
    r5->value = L"存在你账号的加密账本里（AES-256-GCM），不进日志";

    auto* r6 = sec->emplace<SettingRow>();
    r6->key = L"明文文件";
    r6->value = L"users.db 明文，只放用户名 / 哈希 / salt，不含任何金额";

    // ---------------- 分类概况 ----------------
    int expense_count = 0;
    int income_count = 0;
    for (const auto& c : app.categories) {
        if (c.direction == Direction::Expense) ++expense_count;
        else ++income_count;
    }

    auto* cat = section(col, L"分类管理",
                        std::to_wstring(expense_count + income_count) + L" 个 · 点击切换");

    auto* cat_note = cat->emplace<InfoNote>();
    cat_note->text = L"高亮 = 记账时可选，灰色 = 已停用。预置分类不能删（历史记录"
                     L"还引用着），但可以停用。";

    // 两个方向的分类分开列，避免 52 个格子糊成一片
    for (int dir = 0; dir < 2; ++dir) {
        const Direction want = (dir == 0) ? Direction::Expense : Direction::Income;

        auto* head = cat->emplace<Label>();
        head->text = (dir == 0) ? L"支出分类" : L"收入分类";
        head->style = TypeStyle::TitleSmall;

        auto* grid = cat->emplace<Grid>();
        grid->columns = 0;
        grid->min_cell_width = 118.0f;
        grid->gap_x = 8.0f;
        grid->gap_y = 8.0f;
        grid->cell_height = 64.0f;

        for (const auto& c : app.categories) {
            if (c.direction != want) continue;
            auto* chip = grid->emplace<Chip>();
            chip->text = ui::to_wide(c.name);
            chip->cell_height = 64.0f;
            chip->dot = c.is_active ? Color::from_hex(c.color_hex, th.palette.outline)
                                    : th.palette.outline.alpha_of(0.4f);
            chip->selected = c.is_active;
            const std::string id = c.id;
            chip->on_click = [&app, id]() {
                app.toggle_category(id);
                app.rebuild_ui();
            };
        }
    }

    auto* cat_foot = cat->emplace<Label>();
    cat_foot->wrap = true;
    cat_foot->style = TypeStyle::BodySmall;
    cat_foot->color = th.palette.on_surface_variant;
    cat_foot->text = L"截图识别给出的分类名会优先对齐这份列表 —— 候选表越贴合你的实际开销，"
                     L"识别结果越准。所以用不上的分类建议停用而不是放着：候选表短一点，"
                     L"模型选错的余地也小一点。";

    // ---------------- 关于 ----------------
    auto* about = section(col, L"关于");
    auto* about_body = about->emplace<Label>();
    about_body->wrap = true;
    about_body->style = TypeStyle::BodySmall;
    about_body->color = th.palette.on_surface_variant;
    about_body->text =
        L"PenHu Ledger · 原生界面版（v0.2.0-native）\n"
        L"界面：Win32 + Direct2D + DirectWrite，全部自绘，不含任何浏览器 / WebView 组件。\n"
        L"数据：SQLCipher 加密库 + Argon2id 派生密钥 + 内存态会话。\n"
        L"模型：本地 llama-server 与云端 OpenAI 兼容接口共用同一套客户端，"
        L"失败按序降级，全部不可用时回到模板兜底。";
}

}  // namespace penhu::native
