// =============================================================================
//  native/app/shell_common.cpp
//  AppTree（整棵界面树）与平台无关的辅助函数。
//
//  这些原本都在 shell.cpp 里，和 Win32 的消息处理混在一处 ——
//  结果是「想移植界面就必须先把这一千多行里的 145 行挑出来」。
//  挑出来之后再扩平台，就不必再对着一整个文件找哪些能动、哪些不能。
// =============================================================================

#include "app/shell_common.hpp"

#include <algorithm>
#include <string>

#include "app/shell.hpp"
#include "penhu/util/log.hpp"
#include "ui/convert.hpp"
#include "ui/platform_input.hpp"

namespace penhu::native {

/// 诊断模式开关（由 configure_diag 置位）。
///
/// 作用是**抑制错误弹框** —— 无人值守跑自动化时，一个等待点击的模态框
/// 会把整个流程挂死，比不弹更糟。日志照样写。
bool g_diag_mode = false;


/// 收集所有「点了会有反应」的组件（Chip / Button / ListRow）。
void collect_clickables(ui::Widget* w, std::vector<ui::Widget*>& out) {
    if (w == nullptr) return;
    for (const auto& child : w->children()) {
        ui::Widget* c = child.get();
        if (c == nullptr || !c->visible || !c->enabled) continue;
        if (dynamic_cast<ui::Chip*>(c) != nullptr || dynamic_cast<ui::Button*>(c) != nullptr ||
            dynamic_cast<ui::ListRow*>(c) != nullptr) {
            out.push_back(c);
        }
        collect_clickables(c, out);
    }
}

/// 挑一个「点下去状态一定会变」的组件用来注入点击。
///
/// 这里踩过两次坑，都是「验证方法本身不成立」而不是代码有问题：
///   · 第一次按固定坐标点 → 落在 Label 上（不响应点击），根本没走到重建路径；
///   · 第二次改成点第一个 Chip → 它是**已经选中**的那个，点完状态当然不变，
///     日志里却显示「点击没生效」，看着像 bug。
/// 所以现在挑「第一个未选中的分类格」。
ui::Widget* pick_diag_target(ui::Widget* root) {
    std::vector<ui::Widget*> all;
    collect_clickables(root, all);
    for (ui::Widget* c : all) {
        if (auto* chip = dynamic_cast<ui::Chip*>(c)) {
            if (!chip->selected) return c;
        }
    }
    return all.empty() ? nullptr : all.front();
}

/// 挑一个「可以粘文本」的输入框：要非 numeric 的（numeric 框会把
/// "PASTE-CHECK-…" 这种测试串整体拒绝，那样测出来的「没生效」是测试串的问题，
/// 不是粘贴功能的问题 —— 又是一次假的失败信号）。
/// want_password = true 时专门找密码框（API Key 框就是密码框，那是用户
/// 真正要粘贴的地方 —— 测普通框通过不代表密码框也行，密码掩码处理
/// 是另一段代码，漏测过就会出「普通框能粘、密码框不能」的事故）。
ui::TextField* pick_diag_text_field(ui::Widget* w, bool want_password) {
    if (w == nullptr) return nullptr;
    for (const auto& child : w->children()) {
        ui::Widget* c = child.get();
        if (c == nullptr || !c->visible || !c->enabled) continue;
        if (auto* tf = dynamic_cast<ui::TextField*>(c)) {
            if (!tf->numeric && tf->password == want_password) return tf;
        }
        if (auto* deep = pick_diag_text_field(c, want_password)) return deep;
    }
    return nullptr;
}

/// 沿父链冒泡：先给最深命中的节点，它不处理就给父节点。
///
/// **不要退回「只把事件发给最深命中节点」的写法** —— 那样会坏一整类交互：
/// 滚轮落在页面里任何一个 Label / Card 上时，命中的是那个叶子节点，
/// 而它的 on_wheel 是默认空实现，事件就此止步，永远到不了外层的滚动容器。
/// 用户看到的现象就是「界面怎么滚都不动」（这就是当初滚动失效的原因）。
bool bubble_wheel(ui::Widget* hit, const ui::Point& p, float delta) {
    for (ui::Widget* w = hit; w != nullptr; w = w->parent()) {
        if (w->on_wheel(p, delta)) return true;
    }
    return false;
}

/// 按下事件的冒泡版本，返回**真正消费**它的那个组件（没有则 nullptr）。
/// 记下消费者而不是最深命中者，是为了拖动：滚动条的拖动依赖
/// 「按下时抓住滑块，之后所有移动都归它」，而不是靠鼠标恰好停在滑块上。
ui::Widget* bubble_down(ui::Widget* hit, const ui::Point& p) {
    for (ui::Widget* w = hit; w != nullptr; w = w->parent()) {
        if (w->on_pointer_down(p)) return w;
    }
    return nullptr;
}

/// 深度优先找第一个 ScrollView（滚动验证用）
ui::ScrollView* find_scrollview(ui::Widget* w) {
    if (w == nullptr) return nullptr;
    if (auto* sv = dynamic_cast<ui::ScrollView*>(w)) return sv;
    for (const auto& child : w->children()) {
        if (ui::ScrollView* deep = find_scrollview(child.get())) return deep;
    }
    return nullptr;
}
// -----------------------------------------------------------------------------
//  AppTree
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  AppTree
// -----------------------------------------------------------------------------

void AppTree::assemble(App& app) {
    // ---- 顶栏：横跨整个窗口宽度，放在最上面 ----
    // VBox 是「先加的在上面」，所以它必须第一个 add。
    // 横跨全宽而不是只盖内容区：侧边栏上方留一条空白会很怪，
    // 而且窗口的拖动热区本来就该是整条顶边。
    auto title_up = std::make_unique<ui::TitleBar>();
    // 窗口操作走回调转交给 Shell。截图模式用的是局部 AppTree，
    // 那几个回调没人注入 —— 留空即可，点上去什么都不会发生（也不会崩）。
    title_up->on_minimize = [this]() { if (request_minimize) request_minimize(); };
    title_up->on_toggle_maximize = [this]() {
        if (request_toggle_maximize) request_toggle_maximize();
    };
    title_up->on_close = [this]() { if (request_close) request_close(); };
    // 退出登录是应用自己的事，不经过窗口层
    title_up->on_logout = [&app]() { app.logout(); };
    title = title_up.get();
    root.add(std::move(title_up));

    // ---- 主体：左边导航栏 + 右边内容 ----
    auto body_up = std::make_unique<ui::HBox>();
    body_up->flex = 1.0f;

    // 右侧：页面 + 浮层
    auto overlay_up = std::make_unique<ui::Stack>();
    overlay_up->flex = 1.0f;

    auto page_up = std::make_unique<ui::VBox>();
    page_up->flex = 1.0f;
    page_host = page_up.get();

    auto snack_up = std::make_unique<ui::Snackbar>();
    snack_up->visible = false;
    snack = snack_up.get();

    overlay_up->add(std::move(page_up));
    overlay_up->add(std::move(snack_up));
    overlay = overlay_up.get();

    // 左侧：导航栏
    // 注意顺序：HBox 是「先加的在左」，所以侧边栏必须先 add，
    // 否则它会跑到右边去（这个错误一眼就能在截图上看到）。
    auto rail_up = std::make_unique<ui::NavRail>();
    // 图标用几何符号而不是图标字体：这个项目不引入图标字体文件，
    // 而这些符号在 Microsoft YaHei UI 里有字形，不会出现豆腐块。
    rail_up->items = {
        {L"✎", L"记账", false},
        // 「明细」紧跟「记账」：记完一笔最常见的下一步就是翻账本核对
        {L"▤", L"明细", false},
        {L"◫", L"统计", false},
        {L"≡", L"报告", false},
        {L"▣", L"截图", false},
        // 「设置」贴底：它不是主功能之一，和上面四个不该平起平坐
        {L"⌘", L"设置", true},
    };
    rail_up->visible = false;
    rail_up->expanded = app.nav_expanded;
    rail_up->on_select = [&app](int index) {
        static const Screen kMap[6] = {Screen::Entry, Screen::Records, Screen::Stats,
                                       Screen::Report, Screen::Scan, Screen::Settings};
        if (index >= 0 && index < 6) app.go(kMap[index]);
    };
    rail_up->on_toggle = [&app]() {
        app.nav_expanded = !app.nav_expanded;
        app.save_ui_settings();   // 记住这次选择，下次打开保持一致
    };
    rail = rail_up.get();
    body_up->add(std::move(rail_up));   // 先加：占据左边

    // 再加内容区（flex 吃掉剩余宽度）
    auto right_up = std::make_unique<ui::VBox>();
    right_up->flex = 1.0f;
    right_up->add(std::move(overlay_up));
    body_up->add(std::move(right_up));

    root.add(std::move(body_up));
}

void AppTree::rebuild_pages(App& app) {
    if (page_host == nullptr) return;
    page_host->clear_children();

    if (app.token.empty()) {
        build_login_page(*page_host, app);
        return;
    }
    switch (app.screen) {
        case Screen::Entry:    build_entry_page(*page_host, app); break;
        case Screen::Stats:    build_stats_page(*page_host, app); break;
        case Screen::Report:   build_report_page(*page_host, app); break;
        case Screen::Scan:     build_scan_page(*page_host, app); break;
        case Screen::Settings: build_settings_page(*page_host, app); break;
        case Screen::Records:  build_records_page(*page_host, app); break;
        case Screen::Login:    build_login_page(*page_host, app); break;
    }
}

void AppTree::sync(App& app, uint64_t now_ms) {
    static const Screen kMap[6] = {Screen::Entry, Screen::Records, Screen::Stats,
                                   Screen::Report, Screen::Scan, Screen::Settings};
    static const wchar_t* kNames[6] = {L"记账", L"明细", L"统计", L"报告", L"截图", L"设置"};

    if (title != nullptr) {
        // 顶栏左边那一句就是当前页名 —— 和侧边栏的选中项是同一个来源，
        // 所以不会出现「侧边栏高亮统计、顶栏写着报告」这种不一致。
        bool named = false;
        for (int i = 0; i < 6; ++i) {
            if (kMap[i] == app.screen && !app.token.empty()) {
                title->title = kNames[i];
                named = true;
            }
        }
        if (!named) title->title = L"PenHu Ledger";   // 登录页显示应用名
        title->account = app.username;
        title->show_account = !app.token.empty();
    }

    if (rail != nullptr) {
        rail->visible = !app.token.empty();
        // 登录页没有导航（还没进主界面）；其余页面按映射选中对应项
        rail->selected = -1;
        for (int i = 0; i < 6; ++i) {
            if (kMap[i] == app.screen) rail->selected = i;
        }
        rail->expanded = app.nav_expanded;
    }

    if (snack != nullptr) {
        if (app.busy) {
            // 忙碌时复用提示条显示进度，并把点号做成动画 ——
            // 一个静止的「识别中…」会让人以为程序卡死了。
            std::wstring text = app.busy_text;
            const int dots = static_cast<int>((now_ms / 450ull) % 4ull);
            for (int i = 0; i < dots; ++i) text += L"·";
            snack->text = text;
            snack->is_error = false;
            snack->visible = true;
        } else {
            snack->text = app.toast;
            snack->is_error = app.toast_error;
            snack->visible = app.toast_alive(now_ms);
        }
    }
}

// -----------------------------------------------------------------------------
//  Shell
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  截图模式的演示账号
// -----------------------------------------------------------------------------

/// 准备一个可截图的账号：注册（幂等）+ 登录 + 灌入示例记录。
bool prepare_demo_account(App& app) {
    const std::string user = "ui_demo";
    const std::string pass = "Ui-Demo-2026";

    // 已存在会返回 AlreadyExists，属于预期情况，不当错误处理
    (void)app.ledger()->register_user(user, pass);

    auto login = app.ledger()->login(user, pass);
    if (login.is_err()) {
        Logger::default_logger().error("截图模式登录失败：" + login.error().message);
        return false;
    }
    app.token = login.value().token;
    app.username = L"演示账号";
    app.refresh();

    if (!app.records.empty()) return true;

    std::vector<std::string> expense_ids;
    std::vector<std::string> income_ids;
    for (const auto& c : app.categories) {
        if (c.direction == Direction::Expense && c.is_active && c.name != "其他") {
            expense_ids.push_back(c.id);
        } else if (c.direction == Direction::Income && c.is_active && c.name != "其他收入") {
            income_ids.push_back(c.id);
        }
    }
    if (expense_ids.empty()) return true;

    const Date today = Date::today();
    uint32_t seed = 20260920u;
    const wchar_t* notes[] = {L"午饭", L"地铁", L"咖啡", L"打车", L"超市", L"话费",
                              L"电影票", L"感冒药", L"买书", L"停车费"};

    for (int back = 29; back >= 0; --back) {
        const int per_day = static_cast<int>(next_pseudo(seed) % 3u);   // 0..2 笔
        for (int k = 0; k <= per_day; ++k) {
            RecordInput in;
            const int64_t yuan = 8 + static_cast<int64_t>(next_pseudo(seed) % 180u);
            in.amount = Money::from_yuan(yuan);
            in.direction = Direction::Expense;
            in.category_id = expense_ids[next_pseudo(seed) % expense_ids.size()];
            in.date = today.add_days(-back);
            const size_t ni = next_pseudo(seed) % (sizeof(notes) / sizeof(notes[0]));
            in.note = ui::to_utf8(notes[ni]);
            (void)app.ledger()->add_record(app.token, in);
        }
    }

    // 两笔收入，让「本月结余」不是负数
    if (!income_ids.empty()) {
        for (int k = 0; k < 2; ++k) {
            RecordInput in;
            in.amount = Money::from_yuan(k == 0 ? 8200 : 600);
            in.direction = Direction::Income;
            in.category_id = income_ids[k % income_ids.size()];
            in.date = today.add_days(-(5 + k * 10));
            in.note = ui::to_utf8(k == 0 ? L"工资" : L"兼职");
            (void)app.ledger()->add_record(app.token, in);
        }
    }

    app.refresh();
    return true;
}

}  // namespace penhu::native
