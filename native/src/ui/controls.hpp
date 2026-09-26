#pragma once
// =============================================================================
//  native/ui/controls.hpp
//  M3 风格组件库。全部自绘。
//
//  约定（三件事，全库统一）：
//
//  1) 颜色为「未设置」时用 alpha=0 表示 —— 组件内部会回落到主题的对应角色。
//     这样调用方只需要在「要覆盖默认」时才写颜色，不用每个组件都传。
//
//  2) 所有可点组件的最小高度受 Theme::kTouchTarget（48）约束。
//     触控目标是 M3 的硬指标，不是美观问题，所以在 preferred_height 里兜底，
//     而不是靠调用方记得给够高度。
//
//  3) 交互反馈走 on_pointer_down / on_pointer_up 两个回调，
//     按下→抬起都落在同一个组件上才算一次点击（和系统的按钮语义一致）。
//     不在这里做「长按 / 拖拽」，那属于交互逻辑该在页面层写的东西。
// =============================================================================

#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

namespace penhu::native::ui {

/// 未设置颜色的哨兵：alpha==0
inline bool color_unset(const Color& c) { return c.a() == 0u; }

// -----------------------------------------------------------------------------
//  文本
// -----------------------------------------------------------------------------

class Label : public Widget {
public:
    std::wstring text;
    TypeStyle    style{TypeStyle::BodyMedium};
    Color        color{0x00000000u};      // 未设置 → on_surface
    TextAlign    align{TextAlign::Left};
    bool         wrap{true};
    bool         ellipsis{false};
    VAlign       valign{VAlign::Top};

    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
};

/// 左图标 + 主标题 + 副标题 + 右侧金额。记录列表和报告列表共用。
class ListRow : public Widget {
public:
    std::wstring title;
    std::wstring subtitle;      // 可空
    std::wstring trailing;      // 右侧金额/时间，可空
    std::wstring leading_glyph; // 左侧圆形里的符号，可空
    Color        dot{0x00000000u};
    Color        trailing_color{0x00000000u};
    std::function<void()> on_click;
    bool selected{false};

    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_move(const Point& p, bool inside) override;
    bool  on_pointer_down(const Point&) override;
    bool  on_pointer_up(const Point&) override;

private:
    bool pressed_{false};
    bool hover_{false};
};

// -----------------------------------------------------------------------------
//  卡片（圆角容器，内部子节点垂直排列）
// -----------------------------------------------------------------------------

class Card : public VBox {
public:
    Color bg{0x00000000u};     // 未设置 → surface_container_low
    float radius{Theme::kCornerLg};
    bool  outlined{false};
    void  paint(Renderer& r, const Theme& th) override;
};

// -----------------------------------------------------------------------------
//  按钮
// -----------------------------------------------------------------------------

class Button : public Widget {
public:
    enum class Variant {
        Filled,     // 主色填充（页面主操作）
        Tonal,      // 次要容器色（次级操作）
        Outlined,   // 描边
        Text,       // 纯文字
        Danger,     // 错误色填充（删除类操作）
    };

    std::wstring label;
    std::wstring glyph;         // 可选前缀符号
    Variant      variant{Variant::Filled};
    TypeStyle    style{TypeStyle::LabelLarge};
    float        min_height{Theme::kTouchTarget};
    bool         block{false};  // true = 撑满可用宽度
    std::function<void()> on_click;

    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_move(const Point&, bool inside) override;
    bool  on_pointer_down(const Point&) override;
    bool  on_pointer_up(const Point&) override;
    bool  on_key(unsigned vk, bool down) override;

private:
    bool hover_{false};
    bool pressed_{false};
};

// -----------------------------------------------------------------------------
//  分类 chip（网格里的可选项）
// -----------------------------------------------------------------------------

class Chip : public Widget {
public:
    std::wstring text;
    Color        dot{0x00000000u};
    bool         selected{false};
    std::function<void()> on_click;
    /// 网格里一格的高度。72 是「色点 24 + 间距 8 + 文字 20 + 上下留白」算出来的。
    float cell_height{72.0f};

    float preferred_height(float, Renderer&) override { return cell_height; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_move(const Point&, bool inside) override;
    bool  on_pointer_down(const Point&) override;
    bool  on_pointer_up(const Point&) override;

private:
    bool hover_{false};
    bool pressed_{false};
};

// -----------------------------------------------------------------------------
//  分段控件（支出 / 收入 这类二选一）
// -----------------------------------------------------------------------------

class SegmentedTabs : public Widget {
public:
    std::vector<std::wstring> items;
    int selected{0};
    std::function<void(int)> on_change;

    float preferred_height(float, Renderer&) override { return 48.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_move(const Point&, bool inside) override;

private:
    int hover_{-1};
};

// -----------------------------------------------------------------------------
//  底部导航
// -----------------------------------------------------------------------------

class NavBar : public Widget {
public:
    struct Item {
        std::wstring glyph;
        std::wstring label;
    };
    std::vector<Item> items;
    int selected{0};
    std::function<void(int)> on_change;

    float preferred_height(float, Renderer&) override { return 68.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_move(const Point&, bool inside) override;

private:
    int hover_{-1};
};

// -----------------------------------------------------------------------------
//  输入框
// -----------------------------------------------------------------------------

class TextField : public Widget {
public:
    std::wstring placeholder;
    std::wstring value;
    std::wstring supporting;   // 下方辅助说明
    std::wstring prefix;       // 框内前缀（金额框的 "¥"）
    bool         password{false};
    bool         multiline{false};
    bool         numeric{false};   // 只收数字和小数点
    TypeStyle    style{TypeStyle::BodyLarge};
    std::wstring error;            // 非空 → 错误态（红框 + 红字）

    std::function<void()> on_change;
    std::function<void()> on_submit;

    TextField() { focusable = true; }

    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_key(unsigned vk, bool down) override;
    bool  on_text(wchar_t ch) override;
    bool  on_edit_command(EditCmd cmd) override;
    void  on_focus_lost() override;

    /// 光标是否可见（闪烁由 shell 每 500ms 翻转）
    void  tick_caret() { caret_on_ = !caret_on_; }
    size_t caret() const { return caret_; }

    // ---------------------------------------------------------------------
    //  选区
    //
    //  没有单独的「有没有选区」标志位：anchor_ 与 caret_ 相等即表示无选区。
    //  两个成员而不是一个，是为了让「Shift+方向键扩展 / 收起」天然成立 ——
    //  锚点不动、光标动，就是扩展；两者合一，就是取消选中。
    // ---------------------------------------------------------------------
    bool         has_selection() const;
    size_t       sel_begin() const;   ///< 选区靠左一端
    size_t       sel_end() const;     ///< 选区靠右一端
    std::wstring selected_text() const;
    void         select_all();

private:
    /// 把文本插到光标处（先删掉选区）。numeric 框会先净化再校验整串。
    /// 返回 false = 内容不被接受（原样不动，不静默改数据）。
    bool insert_text(const std::wstring& text);
    /// 删掉选区，返回是否真的删掉了东西
    bool delete_selection();
    /// 校验「整串」是否合法（numeric 的硬约束靠它，而不是逐字符判断）
    bool accepts(const std::wstring& s) const;
    /// 改动之后的统一收尾：折叠选区 + 记下改变
    void after_change();

    size_t caret_{0};
    size_t anchor_{0};
    bool   hover_{false};
    bool   caret_on_{true};
    float  field_height_{56.0f};
    Rect   field_rect_{};
};

// -----------------------------------------------------------------------------
//  图表
// -----------------------------------------------------------------------------

/// 每日支出柱状图。values 已经是 0..1 的归一化值。
class BarChart : public Widget {
public:
    std::vector<float> values;
    std::vector<std::wstring> labels;   // 可空；非空时画在下方
    Color bar{0x00000000u};
    Color bar_alt{0x00000000u};        // 最后一项高亮（今天）
    int   highlight_last{-1};          // -1 不高亮
    float height{150.0f};

    float preferred_height(float, Renderer&) override { return height + (labels.empty() ? 0.0f : 24.0f); }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
};

/// 分类占比横条（把若干段按比例拼成一条）
class ShareBar : public Widget {
public:
    struct Segment {
        float weight{0.0f};
        Color color{};
    };
    std::vector<Segment> segments;
    float height{12.0f};

    float preferred_height(float, Renderer&) override { return height; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
};

/// 分类占比里的一行（色点 + 名称 + 金额 + 百分比 + 细条）
class CategoryBar : public Widget {
public:
    std::wstring name;
    std::wstring amount;
    float        share{0.0f};
    Color        color{0x00000000u};

    float preferred_height(float, Renderer&) override { return 46.0f; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;
};

// -----------------------------------------------------------------------------
//  浮层
// -----------------------------------------------------------------------------

/// 提示条。自己定位到屏幕底部居中，所以直接放进 Stack 覆层即可。
class Snackbar : public Widget {
public:
    /// 提示条是**纯视觉层**：点击一律穿透到下面的组件。
    /// 它是通知，不是对话框 —— 用户弹提示时往往正在做别的事（滚列表、点分类、
    /// 切页面），如果它吃掉点击，表现就是「弹个提示之后界面点不动」。
    Snackbar() { pointer_transparent = true; }

    std::wstring text;
    bool  is_error{false};

    float preferred_height(float, Renderer&) override { return 56.0f; }
    void  layout(const Rect& area, Renderer&) override;
    void  paint(Renderer& r, const Theme& th) override;
};

/// M3 开关。
///
/// 滑块位置用 tick 插值过渡（120ms），不是瞬间跳 —— 开关这种高频小动作，
/// 有过渡才不至于看起来像画面闪了一下。
class Switch : public Widget {
public:
    bool value{false};
    bool enabled{true};
    std::function<void(bool)> on_change;

    /// 轨道尺寸。48dp 是触控目标（整行都可点），轨道本身按 M3 是 52×32。
    static constexpr float kTrackW = 52.0f;
    static constexpr float kTrackH = 32.0f;

    Switch() { focusable = true; }

    float preferred_height(float, Renderer&) override { return Theme::kTouchTarget; }
    void  layout(const Rect& area, Renderer&) override;
    bool  tick(float dt_ms) override;
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_move(const Point& p, bool inside) override;
    bool  on_pointer_up(const Point& p) override;
    /// 空格 / 回车也能切换（键盘可达性）
    bool  on_key(unsigned vk, bool down) override;

    Rect track_rect() const;   // 命中测试用（公开给诊断做断言）

private:
    bool  pressed_{false};
    bool  hover_{false};
    /// 0..1 的过渡进度，0 = 关，1 = 开
    float anim_{0.0f};
    bool  anim_init_{false};
};

/// 左侧导航栏（平板/桌面形态）。
///
/// 底部 tab 是手机形态：屏幕小的时候把导航挤到拇指够得到的地方才合理。
/// 宽屏上它的代价是明显的 —— 横向空间被切成一条窄带、文字要挤在图标下面、
/// 设置这类低频入口和主功能平起平坐。侧边栏把这些都理顺：
/// 竖排、图标配文字、低频项贴底、还能折叠起来给内容让位。
class NavRail : public Widget {
public:
    struct Item {
        std::wstring glyph;
        std::wstring label;
        /// 贴到底部的项（设置）。它与主体项之间画一条分隔线 ——
        /// 「设置」和「记账/统计/报告/截图」不是一类东西，不该混在一列里。
        bool pinned_bottom{false};
    };

    std::vector<Item> items;
    int  selected{-1};        // items 的下标；-1 = 没有选中
    bool expanded{true};
    std::function<void(int)> on_select;
    std::function<void()>    on_toggle;

    static constexpr float kExpandedW  = 208.0f;
    static constexpr float kCollapsedW = 68.0f;
    static constexpr float kItemH      = 48.0f;
    static constexpr float kToggleH    = 48.0f;

    NavRail();

    float preferred_height(float, Renderer&) override;
    void  layout(const Rect& area, Renderer&) override;
    void  paint(Renderer& r, const Theme& th) override;
    bool  tick(float dt_ms) override;
    bool  on_pointer_move(const Point& p, bool inside) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_up(const Point& p) override;

    /// 某一项的矩形（诊断/测试用）
    Rect item_rect(int index) const;
    /// 折叠按钮的矩形
    Rect toggle_rect() const { return toggle_rect_; }

private:
    /// 当前宽度（展开与折叠之间插值），同时作为布局用的 width_hint
    float             anim_w_{kExpandedW};
    bool              anim_init_{false};
    Rect              toggle_rect_{};
    std::vector<Rect> rects_;
    /// -1 = 没有悬停, -2 = 悬停在折叠按钮上, >=0 = 悬停在某一项上。
    /// 初始必须是 -1：写成 -2 的话折叠按钮会一直显示悬停背景，
    /// 看起来像「一直按着它」。
    int               hover_{-1};
    bool              pressed_{false};
};

/// 进度条（不确定进度）。value < 0 表示「不确定」，画滑动条。
class ProgressBar : public Widget {public:
    float value{-1.0f};
    float height{6.0f};

    float preferred_height(float, Renderer&) override { return height; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer& r, const Theme& th) override;

    /// 不确定进度动画用的相位（shell 每帧 +1）
    void tick() { phase_ += 0.012f; if (phase_ > 1.0f) phase_ -= 1.0f; }

private:
    float phase_{0.0f};
};

}  // namespace penhu::native::ui
