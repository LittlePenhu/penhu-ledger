// =============================================================================
//  native/ui/titlebar.cpp
// =============================================================================

#include "ui/titlebar.hpp"

#include <algorithm>

#include "ui/renderer.hpp"
#include "penhu/util/log.hpp"

namespace penhu::native::ui {

namespace {

/// 右侧三连按钮左边的留白（把「退出登录」和窗口控制按钮分开）
constexpr float kGapBeforeControls = 20.0f;
/// 「退出登录」的固定宽度。用固定值而不是按文字量测量：
/// 测量需要 Renderer，而 hot_at() / hit_for_drag() 都要在窗口过程里被调用，
/// 那里拿不到渲染器。4 个汉字加左右内边距正好这个宽度。
constexpr float kLogoutW = 64.0f;
/// 用户名和「退出登录」之间的间距。给足一点 ——
/// 挨太近会被读成一句话（「ui_demo 退出登录」），那两个是不同性质的东西：
/// 一个是身份，一个是操作。
constexpr float kGapBeforeLogout = 16.0f;

/// 用户名显示区的最大宽度。名字长了就省略号 ——
/// 顶栏是固定 48dp 高的单行，不能让它把页名挤没。
constexpr float kAccountMaxW = 180.0f;

/// 顶栏左内边距。18 而不是 16：页名第一个字是汉字，字面本身有留白，
/// 用 16 会显得比右侧对称位置更贴边。
constexpr float kPadLeft = 18.0f;

/// 图标线条粗细。1.1dp 在 150% 缩放下是 1.65px，D2D 抗锯齿后仍然清晰；
/// 用 1.0 会显得发灰发虚。
constexpr float kIconStroke = 1.1f;
/// 图标的半边长（10x10 的图标）
constexpr float kIconHalf = 5.0f;

}  // namespace

// -----------------------------------------------------------------------------
//  几何
// -----------------------------------------------------------------------------

Rect TitleBar::control_rect(int index) const {
    // index 0 = 最右边那个 = 关闭。Windows 的次序是 [最小化][最大化][关闭]，
    // 所以从右往左数是 关闭(0) → 最大化(1) → 最小化(2)。
    const float r = bounds_.right() - kButtonW * static_cast<float>(index);
    return {r - kButtonW, bounds_.y, kButtonW, bounds_.h};
}

Rect TitleBar::logout_rect() const {
    const float r = bounds_.right() - kButtonW * 3.0f - kGapBeforeControls;
    return {r - kLogoutW, bounds_.y, kLogoutW, bounds_.h};
}

TitleBar::Hot TitleBar::hot_at(const Point& p) const {
    // 顺序要紧：窗口控制按钮优先于其它区域判定。
    // 万一将来有人把「退出登录」的宽度调大压到了按钮上，
    // 先判按钮能保证「点关闭一定关窗口」。
    for (int i = 0; i < 3; ++i) {
        if (control_rect(i).contains(p)) {
            return i == 0 ? Hot::Close : (i == 1 ? Hot::Max : Hot::Min);
        }
    }
    if (show_account && logout_rect().contains(p)) return Hot::Logout;
    return Hot::None;
}

bool TitleBar::hit_for_drag(const Point& p) const {
    if (!visible || !enabled) return false;
    if (!bounds_.contains(p)) return false;
    // 只有「空白处」能拖：按钮和可点文字上要留给点击。
    // 少了这个判断，点关闭会变成拖窗口 —— 用户会以为关不掉。
    return hot_at(p) == Hot::None;
}

void TitleBar::set_logo_png(std::vector<uint8_t> bytes) {
    logo_png_ = std::move(bytes);
    logo_tried_ = false;   // 新图进来后重新解码
}

void TitleBar::ensure_logo(Renderer& r) {
    if (logo_tried_) return;
    logo_tried_ = true;
    if (logo_png_.empty()) return;
    BitmapRef src;
    if (!r.load_image_mem(logo_png_.data(), logo_png_.size(), src)) {
        Logger::default_logger().warn("顶栏 logo 解码失败");
        return;
    }
    logo_ = src;
}

void TitleBar::layout(const Rect& area, Renderer&) { bounds_ = area; }

// -----------------------------------------------------------------------------
//  绘制
// -----------------------------------------------------------------------------

void TitleBar::draw_icon(Renderer& r, int index, const Color& c) const {
    const Rect box = control_rect(index);
    const float cx = box.x + box.w * 0.5f;
    const float cy = box.y + box.h * 0.5f;

    switch (index) {
        case 2:   // 最小化：一条横线
            r.draw_line({cx - kIconHalf, cy + 0.5f}, {cx + kIconHalf, cy + 0.5f}, c, kIconStroke);
            break;

        case 1:   // 最大化 / 还原 / 退出全屏
            if (fullscreen) {
                // 退出全屏：四个角的对角短线（往内收的意思）。
                // 刻意和「还原」长得不一样 —— 全屏盖住了任务栏，
                // 是比最大化更强的一种状态，图标上必须能区分出来。
                const float d = kIconHalf;          // 角点
                const float in = kIconHalf - 3.2f;  // 另一端的缩进
                r.draw_line({cx - in, cy - d}, {cx - d, cy - in}, c, kIconStroke);
                r.draw_line({cx + in, cy - d}, {cx + d, cy - in}, c, kIconStroke);
                r.draw_line({cx - in, cy + d}, {cx - d, cy + in}, c, kIconStroke);
                r.draw_line({cx + in, cy + d}, {cx + d, cy + in}, c, kIconStroke);
            } else if (maximized) {
                // 还原：前面一个方框，后面那个只露出上边和右边。
                // 两个完整方框套在一起会看不清哪个在前。
                r.stroke_round({cx - kIconHalf, cy - kIconHalf + 3.0f,
                                kIconHalf * 2.0f - 3.0f, kIconHalf * 2.0f - 3.0f},
                               1.5f, c, kIconStroke);
                r.draw_line({cx - kIconHalf + 3.0f, cy - kIconHalf},
                            {cx + kIconHalf, cy - kIconHalf}, c, kIconStroke);
                r.draw_line({cx + kIconHalf, cy - kIconHalf},
                            {cx + kIconHalf, cy + kIconHalf - 3.0f}, c, kIconStroke);
            } else {
                r.stroke_round({cx - kIconHalf, cy - kIconHalf,
                                kIconHalf * 2.0f, kIconHalf * 2.0f}, 1.5f, c, kIconStroke);
            }
            break;

        default:  // 关闭：一个叉
            r.draw_line({cx - kIconHalf, cy - kIconHalf},
                        {cx + kIconHalf, cy + kIconHalf}, c, kIconStroke);
            r.draw_line({cx + kIconHalf, cy - kIconHalf},
                        {cx - kIconHalf, cy + kIconHalf}, c, kIconStroke);
            break;
    }
}

void TitleBar::paint(Renderer& r, const Theme& th) {
    const Palette& p = th.palette;

    // 顶栏和侧边栏同色，合成一个「框架」；内容区用更低的层级。
    // 这样一眼就能看出「哪块是外壳、哪块是内容」——
    // 全用一个颜色的话，深色主题下整块界面会糊成一片。
    r.fill(bounds_, p.surface_container);
    // 一张下边框。深色下顶栏和内容区都很暗，没有这条线两块会糊成一片。
    r.fill({bounds_.x, bounds_.bottom() - 1.0f, bounds_.w, 1.0f}, p.outline_variant);

    // ---- 左侧：页名 ----
    // ---- 应用 logo：和任务栏/exe 用同一张图。它只是标识，不是按钮 ----
    ensure_logo(r);
    if (logo_ != nullptr) {
        const float lx = bounds_.x + 16.0f;
        const float ly = bounds_.y + (bounds_.h - 20.0f) * 0.5f;
        r.draw_image(bitmap_raw(logo_), {lx, ly, 20.0f, 20.0f}, 6.0f, false);
    }

    const float title_x = bounds_.x + kPadLeft + 20.0f + 10.0f;   // logo 右侧
    const float title_right = show_account
                                  ? logout_rect().x - kGapBeforeLogout
                                  : bounds_.right() - kButtonW * 3.0f - kGapBeforeControls;
    if (title_right > title_x) {
        r.text(title, {title_x, bounds_.y, title_right - title_x, bounds_.h},
               TypeStyle::TitleMedium, p.on_surface, TextAlign::Left, VAlign::Middle,
               false, true);
    }

    // ---- 右侧：用户名 + 退出登录 ----
    if (show_account) {
        const Rect lo = logout_rect();
        if (!account.empty()) {
            const float acc_right = lo.x - kGapBeforeLogout;
            const float acc_x = std::max(title_x, acc_right - kAccountMaxW);
            if (acc_right > acc_x) {
                r.text(account, {acc_x, bounds_.y, acc_right - acc_x, bounds_.h},
                       TypeStyle::BodySmall, p.on_surface_variant, TextAlign::Right,
                       VAlign::Middle, false, true);
            }
        }
        if (hot_ == Hot::Logout) {
            r.fill_round(lo, Theme::kCornerSm, p.on_surface.alpha_of(
                down_ == Hot::Logout ? Theme::kStatePressed : Theme::kStateHover));
        }
        r.text(L"退出登录", {lo.x, lo.y, lo.w, lo.h}, TypeStyle::LabelLarge,
               hot_ == Hot::Logout ? p.error : p.on_surface_variant,
               TextAlign::Center, VAlign::Middle, false, false);
    }

    // ---- 右侧：窗口控制按钮 ----
    for (int i = 0; i < 3; ++i) {
        const Rect box = control_rect(i);
        const bool is_close = (i == 0);
        const Hot self = is_close ? Hot::Close : (i == 1 ? Hot::Max : Hot::Min);
        Color fg = p.on_surface_variant;

        if (hot_ == self) {
            if (is_close) {
                // 关闭按钮 hover 用红底白叉 —— 这是 Windows 上所有人的肌肉记忆，
                // 不做的话用户会在想关窗口时犹豫。红色用主题里的 error 角色，
                // 不硬编码色值（深浅两套主题各有一个合适的红）。
                r.fill(box, p.error);
                fg = p.on_error;
            } else {
                r.fill(box, p.on_surface.alpha_of(
                    down_ == self ? Theme::kStatePressed : Theme::kStateHover));
                fg = p.on_surface;
            }
        }
        draw_icon(r, i, fg);
    }
}

// -----------------------------------------------------------------------------
//  交互
// -----------------------------------------------------------------------------

bool TitleBar::on_pointer_down(const Point& p) {
    if (!bounds_.contains(p)) return false;
    down_ = hot_at(p);
    hot_ = down_;
    // 空白处也消费：这是标题栏，点它不该穿透到下面的内容
    return true;
}

bool TitleBar::on_pointer_move(const Point& p, bool inside) {
    const Hot before = hot_;
    hot_ = inside ? hot_at(p) : Hot::None;
    // 返回「是否需要重绘」：鼠标从「最小化」滑到「关闭」时，
    // 命中的组件没变（还是 TitleBar），但高亮的按钮变了 ——
    // 不回 true 的话高亮会卡在原来那个按钮上。
    return hot_ != before;
}

bool TitleBar::on_pointer_up(const Point& p) {
    const Hot up = hot_at(p);
    const Hot was_down = down_;
    down_ = Hot::None;
    if (up != was_down || up == Hot::None) return false;

    switch (up) {
        case Hot::Min:    if (on_minimize) on_minimize(); break;
        case Hot::Max:    if (on_toggle_maximize) on_toggle_maximize(); break;
        case Hot::Close:  if (on_close) on_close(); break;
        case Hot::Logout: if (on_logout) on_logout(); break;
        default: break;
    }
    return true;
}

}  // namespace penhu::native::ui
