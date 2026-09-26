#pragma once
// =============================================================================
//  native/ui/foundation.hpp
//  几何、颜色、设计令牌（M3 design tokens）。
//
//  整个原生界面共享这一份「词汇表」：
//    · 布局代码只用 Rect / Insets / Size 描述位置，不出现裸的 x/y 数值对；
//    · 颜色只从 Palette 的「角色槽位」里取，组件里不写死色值 ——
//      换主题就是把 Palette 换一套，组件一行不改（和原来 web 版的约定一致）。
//
//  坐标单位是 DIP（96 DPI 基准），由渲染器负责乘 DPI 缩放。
//  这样布局代码在 125% / 150% 缩放的屏上不用改任何数字。
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace penhu::native::ui {

// -----------------------------------------------------------------------------
//  几何
// -----------------------------------------------------------------------------

struct Point {
    float x{0.0f};
    float y{0.0f};
};

struct Size {
    float w{0.0f};
    float h{0.0f};
};

struct Insets {
    float l{0.0f};
    float t{0.0f};
    float r{0.0f};
    float b{0.0f};

    static Insets all(float v) { return {v, v, v, v}; }
    static Insets sym(float vertical, float horizontal) { return {horizontal, vertical, horizontal, vertical}; }
    float horizontal() const { return l + r; }
    float vertical() const { return t + b; }
};

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float w{0.0f};
    float h{0.0f};

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Size  size() const { return {w, h}; }
    Point origin() const { return {x, y}; }
    bool  empty() const { return w <= 0.0f || h <= 0.0f; }
    float center_x() const { return x + w * 0.5f; }
    float center_y() const { return y + h * 0.5f; }

    bool contains(const Point& p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }

    Rect deflate(const Insets& i) const {
        return {x + i.l, y + i.t, w - i.l - i.r, h - i.t - i.b};
    }
    Rect inset(float d) const { return deflate(Insets::all(d)); }
    Rect translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }

    Rect intersect(const Rect& o) const {
        const float x0 = std::max(x, o.x);
        const float y0 = std::max(y, o.y);
        const float x1 = std::min(right(), o.right());
        const float y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }

    bool overlaps(const Rect& o) const {
        return !(o.x >= right() || o.right() <= x || o.y >= bottom() || o.bottom() <= y);
    }
};

// -----------------------------------------------------------------------------
//  颜色
//
//  内部一律 0xAARRGGBB。和 D2D 的 D2D1_COLOR_F 之间由渲染器负责转换，
//  这样布局与组件代码不需要知道 D2D 的存在。
// -----------------------------------------------------------------------------

struct Color {
    uint32_t value{0xFF000000u};

    static constexpr Color from_rgb(uint32_t rgb) { return Color{0xFF000000u | (rgb & 0x00FFFFFFu)}; }
    static constexpr Color from_argb(uint32_t a, uint32_t rgb) {
        return Color{((a & 0xFFu) << 24) | (rgb & 0x00FFFFFFu)};
    }
    static constexpr Color transparent() { return Color{0x00000000u}; }

    uint32_t a() const { return (value >> 24) & 0xFFu; }
    uint32_t r() const { return (value >> 16) & 0xFFu; }
    uint32_t g() const { return (value >> 8) & 0xFFu; }
    uint32_t b() const { return value & 0xFFu; }

    /// 覆盖不透明度（乘上去），用于禁用态
    Color with_alpha(float alpha) const {
        const float cur = static_cast<float>(a()) / 255.0f;
        const float out = std::clamp(cur * alpha, 0.0f, 1.0f);
        return Color{(static_cast<uint32_t>(out * 255.0f + 0.5f) << 24) | (value & 0x00FFFFFFu)};
    }

    /// 设置绝对不透明度
    Color alpha_of(float alpha) const {
        const uint32_t av = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        return Color{(av << 24) | (value & 0x00FFFFFFu)};
    }

    /// 以 t 的比例混到 over 上（t=0 返回 *this，t=1 返回 over）。
    /// 状态层（hover/pressed）就是这么实现的：把前景色按 8%/10% 叠上去。
    Color mix(const Color& over, float t) const {
        const float k = std::clamp(t, 0.0f, 1.0f);
        auto lerp = [k](uint32_t x, uint32_t y) {
            return static_cast<uint32_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * k + 0.5f);
        };
        return Color{(lerp(a(), over.a()) << 24) | (lerp(r(), over.r()) << 16) |
                     (lerp(g(), over.g()) << 8) | lerp(b(), over.b())};
    }

    /// 用于给图表分类上色：把 "#RRGGBB" 解析成 Color。失败返回 fallback。
    static Color from_hex(const std::string& hex, Color fallback = Color::from_rgb(0x78909C));
};

// -----------------------------------------------------------------------------
//  设计令牌
// -----------------------------------------------------------------------------

/// 色彩角色。名字和 M3 规范一致，和原 web 版 styles.css 里的变量一一对应。
struct Palette {
    Color primary;
    Color on_primary;
    Color primary_container;
    Color on_primary_container;

    Color secondary;
    Color on_secondary;
    Color secondary_container;
    Color on_secondary_container;

    Color tertiary;
    Color on_tertiary;
    Color tertiary_container;
    Color on_tertiary_container;

    Color error;
    Color on_error;
    Color error_container;
    Color on_error_container;

    Color surface;
    Color on_surface;
    Color surface_variant;
    Color on_surface_variant;

    // surface container 阶梯：M3 用它做层次，而不是靠阴影
    Color surface_container_lowest;
    Color surface_container_low;
    Color surface_container;
    Color surface_container_high;
    Color surface_container_highest;

    Color outline;
    Color outline_variant;
    Color inverse_surface;
    Color inverse_on_surface;
    Color scrim;

    // 项目级语义色（M3 没有支出/收入这两个角色）
    Color expense;
    Color income;
    Color good;
    Color warn;
};

/// 字号阶梯（DIP）。line_height 手写而不是交给 DirectWrite 默认行距，
/// 因为默认行距随字体变化，会让「两行备注」这类地方高度算不准。
struct TypeToken {
    float size{14.0f};
    float line_height{20.0f};
    int   weight{400};   // 400 normal / 500 medium / 600 semibold
};

enum class TypeStyle {
    DisplaySmall,
    HeadlineSmall,
    TitleLarge,
    TitleMedium,
    TitleSmall,
    BodyLarge,
    BodyMedium,
    BodySmall,
    LabelLarge,
    LabelMedium,
};

class Theme {
public:
    Palette palette;
    bool    is_dark{false};

    static Theme light();
    static Theme dark();

    TypeToken type(TypeStyle style) const;

    // 形状刻度
    static constexpr float kCornerXs   = 4.0f;
    static constexpr float kCornerSm   = 8.0f;
    static constexpr float kCornerMd   = 12.0f;
    static constexpr float kCornerLg   = 16.0f;
    static constexpr float kCornerXl   = 28.0f;
    static constexpr float kCornerFull = 9999.0f;

    // 状态层不透明度（M3 规定值）
    static constexpr float kStateHover   = 0.08f;
    static constexpr float kStateFocus   = 0.10f;
    static constexpr float kStatePressed = 0.10f;

    /// 触控目标下限。48 是 M3 硬要求，不是建议值 —— 所有可点组件都按它兜底。
    static constexpr float kTouchTarget = 48.0f;

    // 间距刻度（4 的倍数，避免出现 7px / 13px 这种随手数字）
    static constexpr float kSpace1 = 4.0f;
    static constexpr float kSpace2 = 8.0f;
    static constexpr float kSpace3 = 12.0f;
    static constexpr float kSpace4 = 16.0f;
    static constexpr float kSpace5 = 20.0f;
    static constexpr float kSpace6 = 24.0f;
    static constexpr float kSpace8 = 32.0f;
};

/// 文本水平对齐
enum class TextAlign { Left, Center, Right };

/// 文本垂直对齐（盒子比一行高时用）。
/// 放在 foundation 而不是 renderer 里：控件头文件只需要它，
/// 不该为了一个枚举把 d2d/dwrite/wincodec 那堆系统头拉进来。
enum class VAlign { Top, Middle, Bottom };

/// 组件状态（决定画哪层状态覆盖）
enum class VisualState { Normal, Hover, Pressed, Disabled, Focused };

/// 从 VisualState 取状态层不透明度
inline float state_opacity(VisualState s) {
    switch (s) {
        case VisualState::Hover:    return Theme::kStateHover;
        case VisualState::Pressed:  return Theme::kStatePressed;
        case VisualState::Focused:  return Theme::kStateFocus;
        default:                    return 0.0f;
    }
}

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace penhu::native::ui
