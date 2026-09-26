// =============================================================================
//  native/app/shell_window.cpp
//  窗口帧与状态：最大化 / 全屏 / 还原动画 / DPI 换算 / 无边框窗口的尺寸管理。
//
//  从 shell.cpp 拆出来的第二块（另一块是 shell_diag.cpp）。拆分原则：
//  只搬方法、不改一行逻辑；handle() 的 WM_* 消息分发仍留在 shell.cpp ——
//  文档与崩溃排查记录里的「看 shell.cpp 的 WM_SIZE」指的就是那里。
//
//  Windows 平台实现。Linux 侧的对应概念（xdg_toplevel 的 configure / 状态）
//  在 shell_linux.cpp：两边窗口模型差别太大，不强求文件同构。
// =============================================================================

#include "app/shell.hpp"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <typeinfo>

#include "penhu/util/log.hpp"
#include "app/shell_common.hpp"
#include "ui/convert.hpp"
#include "ui/platform_input.hpp"

namespace penhu::native {

namespace {

/// 窗口最大化 / 还原动画的时长。
/// 180ms 是「看得见、又不觉得等」的量级：短于 120ms 基本感知不到动画，
/// 长于 250ms 会觉得窗口在慢吞吞地挪。
constexpr float kWindowAnimMs = 180.0f;

}  // namespace

bool Shell::is_maximized() const {
    // 手动最大化下 IsZoomed 恒为 false（我们不进 zoomed 态），
    // 所以「铺满工作区」这个状态要把自己维护的标志也算进去。
    // NCCALCSIZE 的兜底分支同样用它：万一 zoomed 溜进来（第三方工具
    // 直接 ShowWindow），客户区也会被摆到工作区上，内容不会错位。
    return hwnd_ != nullptr && (manual_max_ || IsZoomed(hwnd_) != FALSE);
}

void Shell::toggle_maximize() {
    if (hwnd_ == nullptr) return;
    if (fullscreen_) {
        // 全屏时这个按钮的语义是「退出全屏」
        toggle_fullscreen();
        return;
    }
    if (manual_max_) {
        // 还原：目标是自己存下的矩形。不能问系统 —— 手动最大化下窗口
        // 根本不在 zoomed 态，rcNormalPosition 记录的是别的东西。
        RECT back = saved_rect_;
        if (back.right <= back.left) {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd_, &wp)) back = wp.rcNormalPosition;
        }
        manual_max_ = false;
        start_window_anim(back, false, false);
    } else {
        GetWindowRect(hwnd_, &saved_rect_);
        manual_max_ = true;
        // 客户区随动画逐帧连续变化，最后一帧就是工作区 ——
        // 没有任何「到位后突变重排」，这正是动画不闪的原因。
        start_window_anim(rect_for_maximize(), true, false);
    }
}

RECT Shell::rect_for_maximize() const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hwnd_ == nullptr ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        return {};
    }
    // 最大化目标 = 工作区本身。最大化是**手动**做的（toggle_maximize），
    // 窗口矩形直接摆到 rcWork：客户区（NCCALCSIZE 返回 0 = 窗口矩形）恰好
    // 铺满工作区，任务栏原样可见。
    // 旧实现动画到「rcWork 外扩一圈」再调 SW_MAXIMIZE，有两个代价：
    //   · 外扩环（底部正好压在任务栏上）没人画，一旦系统隐藏任务栏就是黑条；
    //   · SW_MAXIMIZE 那一下客户区从外扩尺寸突变到工作区尺寸，重排一帧 = 闪一下。
    return mi.rcWork;
}

RECT Shell::rect_for_fullscreen() const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hwnd_ == nullptr ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        return {};
    }
    // rcMonitor 是整个显示器（含任务栏那条），rcWork 才是不含任务栏的工作区。
    // 全屏就该盖住任务栏 —— 这是它和最大化的唯一区别。
    return mi.rcMonitor;
}

void Shell::start_window_anim(const RECT& to, bool end_maximized, bool end_fullscreen) {
    if (hwnd_ == nullptr) return;
    RECT from{};
    GetWindowRect(hwnd_, &from);
    if (to.right <= to.left || to.bottom <= to.top) return;

    // 起点永远是**当前实际矩形**，不是上一次动画的起点 ——
    // 这样动画进行到一半再点一次，是从现在的位置继续走，而不是跳回原点重来。
    win_anim_.from = from;
    win_anim_.to = to;
    win_anim_.start_ms = GetTickCount64();
    win_anim_.end_maximized = end_maximized;
    win_anim_.end_fullscreen = end_fullscreen;
    win_anim_.active = true;
    win_anim_.frames = 0;

    // ★ 动画前把渲染目标一次配到「全程最大尺寸」（逐维取起终点较大者：
    // 放大时终点更大、缩小时起点更大，中间帧两个维度都不会超过它）。
    // 不预分配的话，动画中容量不足的帧就要 destroy+create 一次 16MB 级的
    // WIC 位图 + D2D 软件渲染目标 —— 那一帧必然掉帧，几段动画连起来
    // 就是肉眼可见的抖动。RECT 是物理像素（无边框窗口客户区 == 窗口矩形），
    // 正是 reserve/resize 的单位。
    renderer_.reserve(std::max(from.right - from.left, to.right - to.left),
                      std::max(from.bottom - from.top, to.bottom - to.top));
    win_anim_.builds0 = renderer_.target_builds();

    // 在动之前记下「还原目标」。这一步不能省，也不能放到动画结束之后做：
    // 那时窗口矩形已经被推到最大化尺寸，再问系统拿到的 rcNormalPosition
    // 就是那个被推大的矩形 —— 表现为「从最大化还原后，窗口位置和大小都漂了」。
    win_anim_.saved.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd_, &win_anim_.saved)) {
        win_anim_.saved = {};
        win_anim_.saved.length = sizeof(WINDOWPLACEMENT);
    }
}

bool Shell::tick_window_anim(uint64_t now_ms) {
    if (!win_anim_.active || hwnd_ == nullptr) return false;

    const float elapsed = static_cast<float>(now_ms - win_anim_.start_ms);
    float t = elapsed / kWindowAnimMs;
    if (t < 0.0f) t = 0.0f;
    if (t >= 1.0f) t = 1.0f;

    // 缓出（ease-out cubic）：起步快、结尾慢。
    // 匀速会显得机械，缓入缓出又太慢 —— 窗口动画要的是「被推到位」的感觉。
    const float inv = 1.0f - t;
    const float e = 1.0f - inv * inv * inv;

    const RECT& a = win_anim_.from;
    const RECT& b = win_anim_.to;
    const int x = a.left + static_cast<int>((b.left - a.left) * e);
    const int y = a.top + static_cast<int>((b.top - a.top) * e);
    const int w = (a.right - a.left) +
                  static_cast<int>(((b.right - b.left) - (a.right - a.left)) * e);
    const int h = (a.bottom - a.top) +
                  static_cast<int>(((b.bottom - b.top) - (a.bottom - a.top)) * e);

    // SWP_NOCOPYBITS：不给它，系统会先把**上一帧的像素**拷贝/拉伸到新几何
    // 再发 WM_SIZE —— 合成器恰在「系统拷贝」与「我们重绘」之间取样的话，
    // 看到的就是拉伸错位的中间帧（「重绘错乱」的直接来源）。
    // 规矩：本文件里凡**改变几何**的 SetWindowPos 一律带这个标志。
    SetWindowPos(hwnd_, nullptr, x, y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS);
    ++win_anim_.frames;

    if (t < 1.0f) return true;

    // ---- 到位：现在才真正切状态 ----
    win_anim_.active = false;
    if (win_anim_.end_fullscreen) {
        // 全屏要盖住任务栏，而任务栏本身是置顶的 —— 不提到 TOPMOST 盖不住。
        // 注意这里**不**调用 ShowWindow：切换窗口状态会让系统重新记
        // rcNormalPosition，而全屏期间我们只想让矩形变大、还原目标保持不变。
        fullscreen_ = true;
        SetWindowPos(hwnd_, HWND_TOPMOST, b.left, b.top, b.right - b.left, b.bottom - b.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    } else {
        // 最大化是**手动**的：矩形动画到位就结束了，不需要再切什么系统状态。
        // 旧实现收尾时 SetWindowPlacement(SW_SHOWMAXIMIZED) 把窗口塞进 zoomed 态
        // —— 矩形被系统外扩进显示器之外，任务栏隐藏、外扩环露黑条，
        // 而且客户区在最后一帧从动画尺寸突变到另一套几何（闪一下的根源）。
        // 现在动画终点就是工作区，到位即完成，客户区全程连续。
        manual_max_ = win_anim_.end_maximized;
        fullscreen_ = false;
        // TOPMOST 的摘除条件是「离开全屏」，而不是「回到普通窗口」：
        // 旧条件 (!end_maximized) 漏掉「全屏→最大化」这条路径 ——
        // 置顶没摘的话最大化窗口会一直压在任务栏上面。
        if (!win_anim_.end_fullscreen) {
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    // ★ 强制重算一遍客户区几何。
    //
    // 动画的最后一帧已经把窗口摆到了目标矩形，紧接着再 SetWindowPos 同一个
    // 矩形，Windows 认为「没变化」，于是不发 WM_NCCALCSIZE ——
    // 客户区就停留在**上一个状态**算出来的几何上。
    // 最大化和全屏的窗口矩形只差任务栏那 72px，这个坑一踩一个准：
    // 诊断里「最大化→全屏」量出来客户区还是 2560x1528（工作区），
    // 而「全屏→最大化」量出来还是 2560x1600（整个显示器）—— 整整差一步。
    // SWP_FRAMECHANGED 的作用正是「即使尺寸没变也要重算边框和客户区」。
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    sync_window_state();

    // 动画期间跨过栏位档（单栏 ↔ 多栏）时没重建，这里补上 ——
    // 只在结束时建一次，比每帧建一次轻得多。
    if (pending_rebuild_) {
        pending_rebuild_ = false;
        rebuild_pages_safely();
    }

    // 记录这一段动画的实际表现。
    // 「卡不卡」不能靠感觉：每帧都要重新布局 + 重绘整页，掉帧是真实存在的
    // 风险，只有量出来才知道要不要优化。
    if (elapsed > 0.0f && win_anim_.frames > 0) {
        const float per_frame = elapsed / static_cast<float>(win_anim_.frames);
        Logger::default_logger().info(
            "窗口动画: " + std::to_string(win_anim_.frames) + " 帧 / " +
            std::to_string(static_cast<int>(elapsed)) + "ms  " +
            std::to_string(static_cast<int>(per_frame)) + "ms/帧  约 " +
            std::to_string(static_cast<int>(1000.0f / per_frame)) + " fps  " +
            "渲染目标重建 " +
            std::to_string(renderer_.target_builds() - win_anim_.builds0) + " 次");
    }
    return false;   // 这一帧已经收尾，不再需要重绘驱动
}

void Shell::toggle_fullscreen() {
    if (hwnd_ == nullptr) return;
    if (fullscreen_) {
        if (was_max_before_fullscreen_) {
            // 退出到**最大化**：目标就是工作区（手动最大化），一镜到底。
            manual_max_ = true;
            start_window_anim(rect_for_maximize(), true, false);
        } else {
            manual_max_ = false;
            start_window_anim(rect_before_fullscreen_, false, false);
        }
        sync_window_state();
    } else {
        // 进入全屏之前先存一份。这一步必须在动窗口**之前**做 ——
        // 全屏期间窗口是 normal 态、矩形被改大，系统会把 rcNormalPosition
        // 同步成全屏尺寸；等退出时再问，拿到的还原目标就是错的。
        was_max_before_fullscreen_ = is_maximized();
        if (was_max_before_fullscreen_) {
            // 之前是最大化：记下它的正常矩形，退出时重新最大化回去
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd_, &wp)) rect_before_fullscreen_ = wp.rcNormalPosition;
        } else {
            GetWindowRect(hwnd_, &rect_before_fullscreen_);
        }
        start_window_anim(rect_for_fullscreen(), false, true);
        sync_window_state();
    }
}

void Shell::apply_window_frame() {
    if (hwnd_ == nullptr) return;

    // 圆角。属性号 33 = DWMWA_WINDOW_CORNER_PREFERENCE，取值 2 = DWMWCP_ROUND。
    // 这里**用数字而不是符号常量**：老版本 SDK 的头文件里没有这两个名字，
    // 写符号名会在别的机器上编译不过，而这个项目是要能一键构建的。
    // Win10 及更早的系统不认这个属性，DwmSetWindowAttribute 直接失败 ——
    // 失败就失败，窗口是直角，不影响任何其它功能。
    const DWORD corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));

    // 系统会在窗口外围画一圈 1px 描边。它按**当前 Windows 主题**取色，
    // 和我们自绘的界面配色无关 —— 深色界面碰上浅色系统主题时，
    // 那一圈浅灰线会非常扎眼。改成和顶栏底色一致，等于让描边消失。
    const ui::Color bg = theme_.palette.surface_container;
    const COLORREF cref = RGB(static_cast<BYTE>(bg.r()), static_cast<BYTE>(bg.g()),
                              static_cast<BYTE>(bg.b()));
    DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &cref, sizeof(cref));

    // 关掉 DWM 自带的窗口过渡动画。
    // 实测：无边框窗口根本吃不到它 —— 注入 SW_MAXIMIZE 之后第一帧（16ms）
    // 窗口矩形就已经是最终值，之后十帧纹丝不动。留着它只会和我们自己做的
    // 逐帧动画打架，两套动画叠加的表现就是闪烁和错位。
    const DWORD no_transition = 1;   // TRUE
    DwmSetWindowAttribute(hwnd_, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */,
                          &no_transition, sizeof(no_transition));

    // ★ 强制重算一遍非客户区。不重算的话，窗口带着 WS_CAPTION 出生时的
    // 那次 WM_NCCALCSIZE 走的是默认过程（那时 GWLP_USERDATA 还没挂上，
    // 我们的「非客户区 = 0」分支不在场），系统按「有标题栏」把客户区
    // 缩了一圈 —— 顶栏上方叠出一条系统标题栏。SWP_FRAMECHANGED 让
    // Windows 就算几何没变也重新问一遍 WM_NCCALCSIZE，我们的分支才接管。
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

ui::Point Shell::to_dip(int px, int py) const {
    const float s = dpi_scale();
    return {static_cast<float>(px) / s, static_cast<float>(py) / s};
}


void Shell::sync_window_state() {
    if (tree_.title == nullptr) return;
    tree_.title->maximized = is_maximized();
    tree_.title->fullscreen = fullscreen_;
}



}  // namespace penhu::native
