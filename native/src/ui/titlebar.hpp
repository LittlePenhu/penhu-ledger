#pragma once
// =============================================================================
//  native/ui/titlebar.hpp
//  应用自定义顶栏 —— 替代 Windows 系统标题栏。
//
//  为什么需要它：系统标题栏用不了应用自己的配色和排版（那是一条
//  跟着系统主题走的灰色条，和 M3 的 surface 阶梯放在一起很突兀）。
//
//  ★ 拖动窗口靠的是 HTCAPTION，不是自己算鼠标位移去 SetWindowPos。
//    自己实现拖动会丢掉 Windows 一整套窗口行为：Aero Snap（拖到屏幕边缘吸附）、
//    双击最大化、拖动时的窗口预览、右键系统菜单、拖到顶部的手势。
//    让 WM_NCHITTEST 对空白区返回 HTCAPTION，这些都免费且跟手。
//    代价是：命中测试必须知道「哪块是空白、哪块是按钮」，
//    所以本类要把 hit_for_drag() 暴露给窗口过程。
// =============================================================================

#include <functional>
#include <string>
#include <vector>

#include "ui/renderer.hpp"
#include "ui/widget.hpp"

namespace penhu::native::ui {

class TitleBar : public Widget {
public:
    std::wstring title;        // 当前页名（未登录时是应用名）
    std::wstring account;      // 用户名
    bool  maximized{false};    // 最大化态 → 画「还原」图标
    bool  fullscreen{false};   // 全屏态 → 画「退出全屏」图标（与还原不同）
    // 为什么要分成两个标志：最大化和全屏是**两种**状态，
    // 共用一个图标的话用户看不出自己现在在哪一种里，
    // 而它们的退出方式（点这个按钮）虽然一样，尺寸语义却不同。
    bool  show_account{true};  // 登录页没有账号可显示

    std::function<void()> on_minimize;
    std::function<void()> on_toggle_maximize;
    std::function<void()> on_close;
    std::function<void()> on_logout;

    /// 顶栏高度。48dp 是「能舒服点到、又不像系统标题栏那么厚」的常用值
    /// （M3 的触控目标下限也是 48）。
    static constexpr float kHeight = 48.0f;
    /// 窗口控制按钮的宽度。系统标题栏是 46x32；我们做满高 48，
    /// 让整块区域都可点（Fitts 定律：目标越大越快）。
    static constexpr float kButtonW = 46.0f;

    float preferred_height(float, Renderer&) override { return kHeight; }
    void  layout(const Rect& area, Renderer&) override;
    void  paint(Renderer& r, const Theme& th) override;
    bool  on_pointer_down(const Point& p) override;
    bool  on_pointer_move(const Point& p, bool inside) override;
    bool  on_pointer_up(const Point& p) override;

    /// 这个点能否用来拖窗口？按钮和可点文字上**不行** ——
    /// 否则点「关闭」会变成拖窗口，点「最小化」会开始拖，都是 bug。
    bool  hit_for_drag(const Point& p) const;

    /// 窗口控制按钮的矩形（0=最小化 1=最大化/还原 2=关闭），从右往左数
    Rect  control_rect(int index) const;

    /// 顶栏 logo 的 PNG 字节（和任务栏/exe 用同一张图，来自 exe 资源）。
    /// 只存字节不解码：解码要 WIC 工厂，在 paint 时惰性做一次。
    void  set_logo_png(std::vector<uint8_t> bytes);

private:
    enum class Hot { None, Min, Max, Close, Logout };

    void  ensure_logo(Renderer& r);

    Rect  logout_rect() const;
    Hot   hot_at(const Point& p) const;
    void  draw_icon(Renderer& r, int index, const Color& c) const;

    Hot   hot_{Hot::None};
    Hot   down_{Hot::None};

    std::vector<uint8_t>     logo_png_;
    BitmapRef logo_;
    bool                     logo_tried_{false};
};

}  // namespace penhu::native::ui
