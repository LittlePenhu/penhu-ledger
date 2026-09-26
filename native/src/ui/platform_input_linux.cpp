// =============================================================================
//  native/ui/platform_input_linux.cpp
//  platform_input 的 Linux 实现：剪贴板 + 修饰键状态。
//
//  两个能力在 Wayland 上都比 Windows 麻烦，原因值得说清楚：
//
//  1) **剪贴板**。Wayland 的剪贴板是「客户端之间直接传」的协议：
//     写方要持有一个 wl_data_source 并**在进程活着的时候**响应对方的读取请求。
//     也就是说要实现写剪贴板，得自己跑一个事件循环去服务这个 source ——
//     对「复制一段文字」这种需求来说代价太大。
//     实践中的标准做法是交给 wl-clipboard（它有个常驻进程专门干这件事），
//     所以这里调 wl-copy / wl-paste。装了就有、没装就明确失败。
//
//  2) **修饰键状态**。Windows 侧是 GetKeyState() 直接问系统；
//     Wayland 里客户端**拿不到**全局键盘状态（这是有意的安全设计 ——
//     否则任何程序都能记录你在按什么）。所以改成由 shell 在按键事件里
//     维护一份状态，这里只负责读。语义上足够：需要知道 Shift 是否按下的地方
//     （文本选择的扩展、Shift+Tab）都在键盘事件处理过程中。
// =============================================================================

#include <cstdlib>
#include <string>

#include "penhu/util/log.hpp"
#include "penhu/util/process.hpp"
#include "ui/convert.hpp"
#include "ui/keys.hpp"
#include "ui/platform_input.hpp"

namespace penhu::native::ui {

namespace {

/// 由 shell 在收到按键/释放事件时更新。只跟踪修饰键。
bool g_shift_down = false;
bool g_ctrl_down = false;
bool g_alt_down = false;

/// 剪贴板工具：优先 wl-clipboard（Wayland 原生），退到 xclip / xsel
/// （走 XWayland，在纯 Wayland 会话里不工作，但至少在 X11 会话里能用）。
std::string find_clip_tool(bool for_paste) {
    if (for_paste) {
        for (const char* t : {"wl-paste", "xclip"}) {
            if (process::command_exists(t)) return t;
        }
        return {};
    }
    for (const char* t : {"wl-copy", "xclip"}) {
        if (process::command_exists(t)) return t;
    }
    return {};
}

void warn_once_no_clipboard() {
    static bool warned = false;
    if (warned) return;
    warned = true;
    Logger::default_logger().warn(
        "剪贴板不可用：没找到 wl-clipboard（Arch: sudo pacman -S wl-clipboard）。"
        "X11 会话下装 xclip 也行。");
}

}  // namespace

/// 供 shell 调用：更新修饰键状态。**放一份状态在这里**是因为
/// Wayland 不允许客户端查询全局键盘状态（见文件头说明）。
void set_modifier_state(bool shift, bool ctrl, bool alt) {
    g_shift_down = shift;
    g_ctrl_down = ctrl;
    g_alt_down = alt;
}

std::wstring clipboard_read_text() {
    const std::string tool = find_clip_tool(true);
    if (tool.empty()) {
        warn_once_no_clipboard();
        return {};
    }

    std::vector<std::string> args;
    if (tool == "wl-paste") {
        // --no-newline：wl-paste 默认会在末尾补一个换行，
        // 而剪贴板里原本没有的话，粘进输入框就多一个字符。
        args = {"--no-newline"};
    } else {
        args = {"-selection", "clipboard", "-o"};
    }

    auto r = process::run(tool, args, {}, 3000);
    if (r.is_err() || r.value().exit_code != 0) return {};
    // 剪贴板里存的本来就是 UTF-8 文本
    return to_wide(r.value().output);
}

bool clipboard_write_text(const std::wstring& text) {
    if (text.empty()) return false;
    const std::string tool = find_clip_tool(false);
    if (tool.empty()) {
        warn_once_no_clipboard();
        return false;
    }

    // 问题：wl-copy 默认会**一直持有**剪贴板直到被新的内容顶掉，
    // 也就是它会 fork 一个后台进程守着。core 的 process::run 是同步等待的，
    // 直接调会一直等到剪贴板被替换 —— 那等于把界面卡死。
    //
    // 所以走 shell 的 `&`：让命令行自己 fork 出去，run 立刻返回。
    // （wl-copy 有这个设计正是为了配合这种用法。）
    const std::string utf8 = to_utf8(text);
    // 用单引号包住内容并把内部的单引号转义掉，避免内容被当命令执行。
    std::string quoted = "'";
    for (char c : utf8) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";

    const std::string cmd = tool == "wl-copy"
                                ? tool + " " + quoted + " >/dev/null 2>&1 &"
                                : tool + " -selection clipboard <<< " + quoted;
    auto r = process::run("/bin/sh", {"-c", cmd}, {}, 3000);
    return r.is_ok() && r.value().exit_code == 0;
}

bool key_down(unsigned key) {
    switch (key) {
        case key::Shift:   return g_shift_down;
        case key::Control: return g_ctrl_down;
        case key::Alt:     return g_alt_down;
        default:
            // 非修饰键的「当前是否按下」在 Wayland 里问不到全局状态。
            // 现有的调用点只问修饰键，所以这里返回 false 是安全的；
            // 将来若真有别的键要问，正确做法是让 shell 一并跟踪而不是假装知道。
            return false;
    }
}

}  // namespace penhu::native::ui
