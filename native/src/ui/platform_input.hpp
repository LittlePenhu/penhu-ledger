#pragma once
// =============================================================================
//  native/ui/platform_input.hpp
//  平台输入辅助：剪贴板 + 修饰键状态。
//
//  为什么单独一个文件、而不是散在各控件里：
//  「粘贴」这件事真正麻烦的地方不是把字符串插进 value，而是
//    · OpenClipboard 可能被别的进程占着（要重试，否则现象是「偶尔粘不上」）；
//    · 从网页/邮件复制来的内容尾随带换行，单行框粘进去会画出框外，
//      而且肉眼完全看不出来（这是最难查的一类故障）；
//    · Ctrl / Shift 的按下状态在 Win32 里没有可靠的单点真相。
//  把这几点集中在这里，控件那边就只剩「插到光标处」一件事。
// =============================================================================

#include <string>

namespace penhu::native::ui {

/// 读剪贴板文本（CF_UNICODETEXT）。没有文本 / 打不开都返回空串。
std::wstring clipboard_read_text();

/// 写剪贴板文本。true = 成功。
bool clipboard_write_text(const std::wstring& text);

/// 单行输入框的粘贴净化：换行折成空格 + 去掉首尾空白。
///
/// 尾随空白必须去掉：从网页或邮件里复制 API Key 常常带一个尾部的 \n，
/// 直接粘进输入框会让 key 校验失败，而框里看上去「一模一样」。
std::wstring sanitize_single_line(const std::wstring& text);

/// 修饰键当前是否按下。
///
/// Windows：直接问系统（GetKeyState）。
/// Linux：Wayland **不允许**客户端查询全局键盘状态（这是有意的安全设计），
///        所以改由 shell 在按键事件里喂进来，这里读一份状态。
///        能问到的只有修饰键 —— 现有的调用点也都只问修饰键。
bool key_down(unsigned key);

#ifndef _WIN32
/// 仅 Linux：由 shell 在每次按键/释放时调用，更新修饰键状态。
///
/// 为什么不做成「事件回调」而是一个全局状态：调用方（TextField 的
/// Shift+方向键选择、Tab 反向切换）是在**处理按键的过程中**问这个问题的，
/// 那时候事件刚好在手，状态天然是最新的。做成回调反而要在每个控件里存一份。
void set_modifier_state(bool shift, bool ctrl, bool alt);
#endif

}  // namespace penhu::native::ui
