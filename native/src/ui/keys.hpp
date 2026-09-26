#pragma once
// =============================================================================
//  native/ui/keys.hpp
//  平台无关的键码。
//
//  为什么数值刻意与 Win32 的 VK_* 一致：
//    · Windows 侧**零转换** —— WM_KEYDOWN 的 wParam 本来就是 VK 码，
//      直接传给 on_key() 即可，不会引入「映射表写错一个键」这类 bug。
//    · Linux 侧只在这一个地方做映射（xkbcommon keysym → 这里），
//      映射表集中在一处，好审也好测。
//
//  之前这份常量是从 windows.h 里"顺手"拿的（renderer.hpp 间接引入），
//  于是 controls.cpp 这种纯共享代码也默默依赖了 Windows 头 ——
//  在 Linux 上编到第 484 行才发现 VK_SHIFT 不存在。
// =============================================================================

namespace penhu::native::ui::key {

// 与 Win32 VK_* 同值
constexpr unsigned Backspace  = 0x08;
constexpr unsigned Tab        = 0x09;
constexpr unsigned Enter      = 0x0D;   // VK_RETURN
constexpr unsigned Shift      = 0x10;
constexpr unsigned Control    = 0x11;
constexpr unsigned Alt        = 0x12;   // VK_MENU
constexpr unsigned Pause      = 0x13;
constexpr unsigned CapsLock   = 0x14;
constexpr unsigned Escape     = 0x1B;
constexpr unsigned Space      = 0x20;
constexpr unsigned PageUp     = 0x21;
constexpr unsigned PageDown   = 0x22;
constexpr unsigned End        = 0x23;
constexpr unsigned Home       = 0x24;
constexpr unsigned Left       = 0x25;
constexpr unsigned Up         = 0x26;
constexpr unsigned Right      = 0x27;
constexpr unsigned Down       = 0x28;
constexpr unsigned Insert     = 0x2D;
constexpr unsigned Delete     = 0x2E;

// 功能键（F11 = 全屏，和 Windows 侧一致）
constexpr unsigned F1  = 0x70;
constexpr unsigned F11 = 0x7A;
constexpr unsigned F12 = 0x7B;

// 字母/数字：VK 值与 ASCII 大写一致，所以 'A'~'Z'、'0'~'9' 可以直接用字符字面量
constexpr unsigned A = 'A';
constexpr unsigned Z = 'Z';
constexpr unsigned Num0 = '0';
constexpr unsigned Num9 = '9';

}  // namespace penhu::native::ui::key
