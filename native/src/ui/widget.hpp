#pragma once
// =============================================================================
//  native/ui/widget.hpp
//  组件基类与布局容器。
//
//  布局模型：**单遍、自上而下、绝对坐标**。
//    · 父容器拿到一个绝对矩形 area，决定每个子节点分别落在哪个绝对矩形里；
//    · 子节点收到的是「已经算好的绝对坐标」，不需要知道自己在第几层。
//
//  为什么不做「两遍测量 + 相对坐标 + 渲染时累加偏移」那套（Flutter/Yoga 的路子）：
//  这个界面的结构是固定且扁平的（垂直卡片流 + 等宽网格 + 底部导航），
//  单遍绝对坐标足够表达，而且带来一个很值钱的副作用 ——
//  **滚动就是把偏移算进子节点坐标，命中测试不用做任何坐标换算**。
//  滚动容器少写一个 transform 逆变换，就少一类「点到了、但点在错的地方」的 bug。
//
//  布局需要 Renderer（文本高度必须实测，不能估），所以 layout/paint 都收它。
//  这两件事都不依赖 BeginDraw，测量用的 DirectWrite 工厂是常驻的。
// =============================================================================

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/foundation.hpp"

namespace penhu::native::ui {

class Renderer;

/// 编辑命令 —— 由 shell 从 Ctrl 组合键翻译过来，交给当前焦点组件。
/// 只有文本输入框认这几个命令，别的组件拿到就无视（见 on_edit_command 的默认实现）。
enum class EditCmd { Copy, Cut, Paste, SelectAll };

class Widget {
public:
    virtual ~Widget() = default;

    // ---------------------------------------------------------------------
    //  树
    // ---------------------------------------------------------------------
    Widget* add(std::unique_ptr<Widget> child);
    template <typename T, typename... Args>
    T* emplace(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = p.get();
        add(std::move(p));
        return raw;
    }
    void clear_children();
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    Widget* parent() const { return parent_; }

    // ---------------------------------------------------------------------
    //  几何
    // ---------------------------------------------------------------------
    const Rect& bounds() const { return bounds_; }
    void set_bounds(const Rect& r) { bounds_ = r; }
    Rect content_bounds() const { return bounds_.deflate(padding); }

    // ---------------------------------------------------------------------
    //  布局
    // ---------------------------------------------------------------------
    /// 在给定宽度下想要多高。必须是纯函数（不修改任何状态）——
    /// 父容器会先问一遍总高，再据此分配剩余空间，中途改状态会让两遍不一致。
    virtual float preferred_height(float width, Renderer& r);
    /// 定位自己并递归定位子节点。area 是父给的绝对矩形。
    virtual void layout(const Rect& area, Renderer& r);
    /// 每帧推进动画（dt_ms 是距上一帧的毫秒数）。
    /// 返回 true 表示这一帧有变化、需要重绘 —— shell 据此决定要不要刷屏，
    /// 静止状态下就不会白白重绘（这个界面是按需重绘的）。
    virtual bool tick(float dt_ms);

    // ---------------------------------------------------------------------
    //  绘制
    // ---------------------------------------------------------------------
    virtual void paint(Renderer& r, const Theme& th) = 0;

    // ---------------------------------------------------------------------
    //  事件。返回 true 表示已消费，不再向上冒泡。
    // ---------------------------------------------------------------------
    virtual bool on_pointer_move(const Point& p, bool inside);
    virtual bool on_pointer_down(const Point& p);
    virtual bool on_pointer_up(const Point& p);
    virtual bool on_wheel(const Point& p, float delta);
    virtual bool on_key(unsigned vk, bool down);
    virtual bool on_text(wchar_t ch);
    virtual void on_focus_lost() {}

    /// 编辑命令。默认返回 false（= 不认），这是刻意的：
    /// 光标停在按钮上时按 Ctrl+V 不该有任何事发生，
    /// 更不该让 shell 把这条按键当成「按钮被按了」。
    virtual bool on_edit_command(EditCmd cmd) {
        (void)cmd;
        return false;
    }

    /// 命中测试：返回包含 p 的最深可见节点（子节点优先，后加的在更上层）
    virtual Widget* hit_test(const Point& p);
    /// 收集可聚焦节点（Tab 键遍历用），按树的先序
    virtual void collect_focusables(std::vector<Widget*>& out);

    // ---------------------------------------------------------------------
    //  状态
    // ---------------------------------------------------------------------
    bool  visible{true};
    bool  enabled{true};
    bool  focusable{false};
    bool  focused{false};
    /// 按下它时**不改变**键盘焦点。
    /// 滚动条是唯一的使用者：正在备注框里打字时去拖滚动条，
    /// 焦点被抢走的话输入光标会跳、输入法未提交的内容会丢 ——
    /// 明明只是滚一下，不该付出这个代价。
    bool  preserve_focus_on_press{false};
    /// 纯视觉层：不参与命中测试，点击直接穿透到它下面的组件。
    /// 提示条（Snackbar）用它。通知弹出期间用户往往正在做别的事，
    /// 若它吃掉点击，表现就是「弹个提示之后整个界面点不动」。
    bool  pointer_transparent{false};
    /// >0 时参与「剩余空间」按比例分配（类似 CSS flex-grow）
    float flex{0.0f};
    /// >0 且父容器可用宽度更大时，限宽并水平居中。
    /// 登录卡片、设置表单这类「不该在宽屏上拉满」的内容靠它居中，
    /// 比再套一层 HBox + 两侧 Spacer 直观，也不会让 preferred_height 算错宽度。
    float max_width{0.0f};
    /// >0 时在 HBox 里直接占这个宽度，不再参与平分。
    /// 用途是工具栏上的按钮：「选择截图」这种短标签按钮如果被平分，
    /// 会变成一个半屏宽的色块，看起来像布局坏了。
    float width_hint{0.0f};
    Insets padding{};
    /// 页面私挂数据的口子。用它避免为了几个字段就派生一个新类。
    void* user{nullptr};

protected:
    Widget* parent_{nullptr};
    std::vector<std::unique_ptr<Widget>> children_;
    Rect bounds_{};
};

// -----------------------------------------------------------------------------
//  容器
// -----------------------------------------------------------------------------

/// 垂直排列。子节点高度由自身 preferred_height 决定，flex>0 的瓜分剩余空间。
class VBox : public Widget {
public:
    float gap{0.0f};
    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;
};

/// 水平排列
class HBox : public Widget {
public:
    float gap{0.0f};
    /// 垂直对齐方式
    enum class Align { Start, Center, End, Stretch };
    Align align{Align::Stretch};
    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;
};

/// 等宽网格（分类选择用）
class Grid : public Widget {
public:
    int   columns{4};
    /// columns == 0 时，按这个最小格宽自动算列数。
    /// 分类多起来之后（预置 40 多个支出分类），固定 4 列会拉出十几行；
    /// 按可用宽度算列数能在宽屏上把网格压短一半。
    float min_cell_width{132.0f};
    float gap_x{8.0f};
    float gap_y{8.0f};
    /// 0 表示按第一行子节点的最大 preferred_height 决定
    float cell_height{0.0f};
    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;

private:
    int resolve_columns(float inner_width) const;
};

/// 层叠：所有子节点占满同一矩形（浮层、遮罩用）
class Stack : public Widget {
public:
    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;
    /// 只有最上层可见的子节点参与命中
    Widget* hit_test(const Point& p) override;
};

/// 弹性空白
class Spacer : public Widget {
public:
    float h{0.0f};
    float w{0.0f};
    float preferred_height(float width, Renderer&) override { return h; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer&, const Theme&) override {}
};

/// 固定高度的分隔空白（比 Spacer 更常用，语义更清楚）
class Gap : public Widget {
public:
    float h{0.0f};
    float preferred_height(float, Renderer&) override { return h; }
    void  layout(const Rect& area, Renderer&) override { bounds_ = area; }
    void  paint(Renderer&, const Theme&) override {}
};

/// 滚动容器。内部内容放 content（一个 VBox），滚动偏移直接算进子节点坐标。
///
/// content 是**一个正常的子节点**，不是游离在 children_ 之外的值成员。
/// 这一点很关键：只要它不在 children_ 里，所有「遍历子节点」的通用逻辑都会
/// 整片漏掉页面内容 —— hit_test / paint / layout 当初各自写了特例所以没出事，
/// 但 collect_focusables（Tab 键轮换焦点）和诊断用的查找就没这么幸运了：
/// 症状是「Tab 键按了没反应」，而且不报任何错。
class ScrollView : public Widget {
public:
    ScrollView();

    /// 内容容器。引用成员，用法和普通成员一样（`scroll->content.xxx`）。
    VBox& content;

    /// 当前显示位置。滚动偏移直接算进子节点坐标，所以它就是「屏幕上的真实位置」。
    float offset{0.0f};
    /// 目标位置。滚轮只改它，实际 offset 每帧向它逼近 ——
    /// 一格一跳会让滚动有「阶段感」，动画之后才是连续的滑动。
    float target_offset{0.0f};
    bool  show_bar{true};
    /// 滚动一格的像素（滚轮一格 = 3 行 ≈ 120）
    float wheel_step{120.0f};

    float preferred_height(float width, Renderer& r) override;
    void  layout(const Rect& area, Renderer& r) override;
    void  paint(Renderer& r, const Theme& th) override;
    bool  tick(float dt_ms) override;
    bool  on_wheel(const Point& p, float delta) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_move(const Point& p, bool inside) override;
    bool  on_pointer_up(const Point& p) override;
    Widget* hit_test(const Point& p) override;

    float content_height() const { return content_height_; }
    float max_offset() const;
    /// 滚到指定位置。immediate=true 用于「切页重置」这类不该有动画的场合。
    void  scroll_to(float value, bool immediate = false);
    void  scroll_to_top() { scroll_to(0.0f, true); }
    /// 让某个子节点进入可视区（键盘 / 程序化滚动用）
    void ensure_visible(const Widget* child);

    /// 滚动条滑块矩形（不可滚时返回空矩形）
    Rect thumb_rect() const;

private:
    /// 建内容容器并挂进 children_，返回裸指针给引用成员绑定
    VBox* make_content();
    /// 滚动条的可抓取范围。**热区必须比视觉宽得多**：滑块画出来只有 4dp，
    /// 拿它当命中区的话鼠标和手指都几乎抓不住，用户遇到的就是「拖了没反应」。
    Rect bar_hit_rect() const;
    /// 按鼠标 y 重算偏移（拖滑块、点轨道都用它）
    void apply_drag(float y);
    /// 把 offset 与 target 一起夹回合法区间（内容高度变化后也要调）
    void clamp_both();

    float content_height_{0.0f};
    bool  dragging_{false};
    /// 抓取点距滑块顶端的距离。拖动过程中保持不变，滑块才不会在光标下跳。
    float drag_grab_dy_{0.0f};
};

}  // namespace penhu::native::ui
