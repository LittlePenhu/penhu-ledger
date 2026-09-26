// =============================================================================
//  native/main_linux.cpp
//  PenHu Ledger —— Linux 入口。
//
//  两个模式（和 Windows 侧一致）：
//    penhu-native                     正常开窗（Wayland）
//    penhu-native --shot <目录>        离屏渲染各页面成 PNG（用于核对排版）
//  其余参数：--data-dir <目录>（不指定就用 $XDG_DATA_HOME/PenHuLedger 或 ~/.local/share/...）
//            --width / --height（截图尺寸，默认 1240x860，与窗口逻辑尺寸一致）
//
//  和 Windows 入口的差别，逐条说明：
//    · 没有 HINSTANCE / wWinMain：Linux 就是普通的 main(argc, argv)。
//    · 没有 DPI 感知设置：Wayland 里缩放由合成器告诉客户端，
//      不存在「进程级 DPI 感知」这种东西（那是 Win32 的历史包袱）。
//    · 没有控制台重定向：Windows 的 GUI 子系统程序默认没 stdout，
//      要从父进程借；Linux 的 stdout 一直都在。
//    · set_terminate 仍然要装，但收尾方式不同：日志 + _Exit，
//      没有 MessageBox 可用（见 shell_linux.cpp 里 report_uncaught 的说明）。
//    · 需要先设一个确定的 locale：字体回溯、数字格式都受它影响。
//      不设的话某些环境下 LANG=C 会让中文显示成问号。
// =============================================================================

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "app/shell.hpp"
#include "penhu/util/log.hpp"
#include "ui/convert.hpp"

using penhu::native::App;
using penhu::native::Shell;
using penhu::native::run_screenshot_mode;
using penhu::native::ui::to_utf8;
using penhu::native::ui::to_wide;

namespace {

struct CmdLine {
    std::string data_dir;
    std::string shot_dir;
    int         width{1240};
    int         height{860};
    // 诊断模式：开窗后自动登录，把窗口那一帧存成 PNG 再退出。
    std::string diag_user;
    std::string diag_pass;
    std::string diag_shot;
    std::string diag_scan_dir;
};

CmdLine parse_cmdline(int argc, char** argv) {
    CmdLine o;
    auto next = [&](int& i) -> std::string {
        return (i + 1 < argc) ? std::string(argv[++i]) : std::string{};
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data-dir") {
            o.data_dir = next(i);
        } else if (a == "--shot") {
            o.shot_dir = next(i);
        } else if (a == "--width") {
            o.width = std::atoi(next(i).c_str());
        } else if (a == "--height") {
            o.height = std::atoi(next(i).c_str());
        } else if (a == "--diag-login") {
            o.diag_user = next(i);
        } else if (a == "--diag-pass") {
            o.diag_pass = next(i);
        } else if (a == "--diag-shot") {
            o.diag_shot = next(i);
        } else if (a == "--diag-scan-dir") {
            o.diag_scan_dir = next(i);
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "用法：penhu-native [选项]\n"
                "  --shot <目录>            离屏渲染各页面成 PNG（排版核对用）\n"
                "  --width / --height <N>   截图尺寸（默认 1240x860）\n"
                "  --data-dir <目录>        数据目录\n"
                "  --diag-login/--diag-pass/--diag-shot  窗口诊断模式\n");
            std::exit(0);
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    // 明确设成 UTF-8：启动时进程的 locale 是 "C"，而 Pango / fontconfig
    // 会按 locale 选字体和回退链 —— LANG=C 时中文可能拿不到 CJK 字体。
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_CTYPE, "C.UTF-8");

    const CmdLine cli = parse_cmdline(argc, argv);

    // ---- 最后一道防线：任何线程上的未捕获 C++ 异常 ----
    // 默认的 std::terminate 直接调 abort()：进程消失、日志空白。
    // 和 Windows 一样装一个自己的 handler，但这里只能写日志 ——
    // Wayland 客户端没有「系统级弹框」，而自己画一个窗口去报错是循环依赖
    // （绘制本身可能正是出问题的那一环）。
    std::set_terminate([]() {
        std::string msg = "std::terminate：未捕获的 C++ 异常";
        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                msg += std::string("（") + e.what() + "）";
            } catch (...) {
                msg += "（非 std::exception 派生类型）";
            }
        } else {
            msg += "（没有异常对象：可能是显式 abort / terminate）";
        }
        penhu::Logger::default_logger().error(msg);
        std::fflush(nullptr);
        // 用 _Exit 而不是 abort：abort 会再走一次 terminate，可能递归。
        // 代价是跳过静态析构（账本连接不优雅关闭）—— SQLite/SQLCipher 靠
        // journal 恢复，不会因此损坏；而「能留下日志」比「优雅退出」重要得多。
        std::_Exit(3);
    });

    // ---- 截图模式：不需要 Wayland 连接，纯离屏 ----
    // 这条也是本机验证界面移植的主要手段：没有图形会话时（比如 ssh 进来）
    // 依然能把每一页渲染出来核对排版。
    if (!cli.shot_dir.empty()) {
        const int rc = run_screenshot_mode(cli.data_dir, cli.shot_dir, cli.width, cli.height);
        App::get().shutdown();
        return rc;
    }

    App& app = App::get();
    std::wstring err;
    if (!app.init(cli.data_dir, &err)) {
        std::fprintf(stderr, "初始化失败：%s\n", to_utf8(err).c_str());
        return 1;
    }

    if (!Shell::get().init(cli.data_dir, &err)) {
        std::fprintf(stderr, "界面初始化失败：%s\n", to_utf8(err).c_str());
        app.shutdown();
        return 1;
    }

    if (!cli.diag_user.empty()) {
        Shell::get().configure_diag(cli.diag_user, cli.diag_pass, cli.diag_shot,
                                    cli.diag_scan_dir);
    }

    const int rc = Shell::get().run();
    app.shutdown();
    return rc;
}
