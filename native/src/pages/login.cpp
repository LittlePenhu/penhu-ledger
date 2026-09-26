// =============================================================================
//  native/pages/login.cpp
//  登录 / 注册页。
// =============================================================================

#include <algorithm>

#include "app/app.hpp"
#include "pages/page_common.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {

using namespace ui;

/// 品牌区：圆标 + 标题 + 副标题。
/// 整块自绘而不套 HBox：这三个元素的宽度互相依赖（标题要占掉圆标右侧的剩余宽度），
/// 用弹性容器反而更难表达，也容易在垂直对齐上偏几个像素。
class BrandHeader : public Widget {
public:
    float preferred_height(float, Renderer&) override { return 64.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }

    void paint(Renderer& r, const Theme& th) override {
        const float d = 52.0f;
        const Rect mark{bounds_.x, bounds_.y + (bounds_.h - d) * 0.5f, d, d};
        r.fill_round(mark, Theme::kCornerMd, th.palette.primary_container);
        r.text_line(L"¥", mark, TypeStyle::HeadlineSmall, th.palette.on_primary_container,
                    TextAlign::Center);

        const float tx = mark.right() + 14.0f;
        const float tw = std::max(40.0f, bounds_.right() - tx);
        r.text_line(L"PenHu Ledger", {tx, bounds_.y + 5.0f, tw, 32.0f}, TypeStyle::TitleLarge,
                    th.palette.on_surface, TextAlign::Left, true);
        r.text_line(L"本机记账 · 数据加密存储", {tx, bounds_.y + 37.0f, tw, 20.0f},
                    TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, true);
    }
};

}  // namespace

void build_login_page(VBox& host, App& app) {
    host.padding = Insets::all(0.0f);
    host.gap = 0.0f;

    // 上下用 Spacer 把卡片压到垂直中间
    auto* top = host.emplace<Spacer>();
    top->flex = 1.0f;

    auto* card = host.emplace<Card>();
    card->max_width = 440.0f;
    card->padding = Insets::all(28.0f);
    card->gap = 14.0f;

    card->emplace<BrandHeader>();

    const bool registering = (app.login_mode == 1);

    auto* seg = card->emplace<SegmentedTabs>();
    seg->items = {L"登录", L"注册"};
    seg->selected = app.login_mode;
    seg->on_change = [&app](int idx) {
        app.login_mode = idx;
        app.login_error.clear();
        app.rebuild_ui();
    };

    auto* user = card->emplace<TextField>();
    user->placeholder = L"用户名";
    user->value = app.login_username;
    user->supporting = registering ? L"3-32 位，字母数字 _ -，须以字母或数字开头" : L"";
    user->style = TypeStyle::BodyLarge;
    user->on_change = [&app, user]() { app.login_username = user->value; };
    user->on_submit = [&app]() {
        app.login_async(ui::to_utf8(app.login_username), ui::to_utf8(app.login_password),
                        app.login_mode == 1);
    };

    auto* pass = card->emplace<TextField>();
    pass->placeholder = L"密码";
    pass->value = app.login_password;
    pass->password = true;
    pass->supporting = registering ? L"至少 8 位，且需含字母、数字、符号中的两类" : L"";
    pass->on_change = [&app, pass]() { app.login_password = pass->value; };
    pass->on_submit = [&app]() {
        app.login_async(ui::to_utf8(app.login_username), ui::to_utf8(app.login_password),
                        app.login_mode == 1);
    };

    if (!app.login_error.empty()) {
        auto* err = card->emplace<ErrorBanner>();
        err->text = ui::to_wide(app.login_error);
    }

    auto* submit = card->emplace<Button>();
    submit->variant = Button::Variant::Filled;
    submit->block = true;
    submit->label = registering ? L"注册并登录" : L"登录";
    submit->enabled = !app.busy;
    submit->on_click = [&app]() {
        app.login_async(ui::to_utf8(app.login_username), ui::to_utf8(app.login_password),
                        app.login_mode == 1);
    };

    auto* foot = card->emplace<Label>();
    foot->text = L"密钥由密码经 Argon2id 派生，只存在于内存；进程退出后需要重新登录。"
                 L"忘记密码无法找回数据 —— 这是加密的必然代价。";
    foot->style = TypeStyle::BodySmall;
    foot->color = app.dark ? Theme::dark().palette.on_surface_variant
                           : Theme::light().palette.on_surface_variant;
    foot->wrap = true;

    auto* bottom = host.emplace<Spacer>();
    bottom->flex = 1.0f;
}

}  // namespace penhu::native
