// =============================================================================
//  native/main.cpp
//  PenHu Ledger —— 原生界面入口。
//
//  两个模式：
//    penhu-native.exe                       正常开窗
//    penhu-native.exe --shot <目录>          离屏渲染各页面成 PNG（用于核对排版）
//  其余参数：--data-dir <目录>（不指定就用 %USERPROFILE%\PenHuLedger）
//            --width / --height（截图尺寸，默认 1240x860，与窗口一致）
// =============================================================================

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "app/app.hpp"
#include "app/shell.hpp"
#include "penhu/util/log.hpp"
#include "ui/convert.hpp"

using penhu::native::App;
using penhu::native::Shell;
using penhu::native::run_screenshot_mode;
using penhu::native::ui::to_utf8;

namespace {

struct CmdLine {
    std::wstring data_dir;
    std::wstring shot_dir;
    int          width{1240};
    int          height{860};
    // 诊断模式：开窗后自动登录，把窗口那一帧存成 PNG 再退出。
    // 正常使用不需要这三个参数。
    std::wstring diag_user;
    std::wstring diag_pass;
    std::wstring diag_shot;
    std::wstring diag_scan_dir;
};

CmdLine parse_cmdline() {
    CmdLine o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return o;

    auto next = [&](int& i) -> std::wstring {
        return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring{};
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--data-dir") {
            o.data_dir = next(i);
        } else if (a == L"--shot") {
            o.shot_dir = next(i);
        } else if (a == L"--width") {
            o.width = _wtoi(next(i).c_str());
        } else if (a == L"--height") {
            o.height = _wtoi(next(i).c_str());
        } else if (a == L"--diag-login") {
            o.diag_user = next(i);
        } else if (a == L"--diag-pass") {
            o.diag_pass = next(i);
        } else if (a == L"--diag-shot") {
            o.diag_shot = next(i);
        } else if (a == L"--diag-scan-dir") {
            o.diag_scan_dir = next(i);
        }
    }
    LocalFree(argv);
    return o;
}

/// 这是个 WINDOWS 子系统程序（双击不带黑框），默认没有 stdout。
/// 截图模式是给自动化用的，需要能看到输出，所以从父进程借一个控制台。
void attach_parent_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) return;
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // ---- 最后一道防线：任何线程上的未捕获 C++ 异常 ----
    // 默认的 std::terminate 直接调 abort()：进程消失、日志空白，
    // 用户只看到「闪退」，我们连崩在哪个功能都不知道。
    // 装一个自己的 handler：写日志 + 弹一次提示，再退出。
    //
    // 三条自我约束（handler 本身出错就是无限递归）：
    //   · 不再抛异常；
    //   · 不用可能已经析构的复杂全局对象（只碰 Logger 这个单例和 Win32 API）；
    //   · 提示文本用 MultiByteToWideChar 而不是项目里的转换工具 ——
    //     这个函数本身在异常路径上，不能再依赖别的可能抛异常的代码。
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

        if (penhu::native::g_diag_mode) _Exit(4);   // 无人值守：不弹框

        wchar_t wbuf[1024] = {0};
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wbuf, 1023);
        MessageBoxW(nullptr, wbuf, L"PenHu 遇到内部错误（详情已写入日志）",
                    MB_OK | MB_ICONERROR);
        // 用 _Exit 而不是 abort：abort 会再走一次 terminate，可能递归。
        // 代价是跳过静态析构（账本连接不优雅关闭）——
        // SQLite/SQLCipher 靠 journal 恢复，不会因此损坏；而「能留下日志」
        // 比「优雅退出」重要得多。
        _Exit(3);
    });

    // 每显示器 DPI 感知 v2：不做的话在 125% / 150% 缩放的屏上，
    // 48dp 的触控目标和圆角会被系统拉伸，看起来像「做得毛糙」。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        // 已经初始化过（RPC_E_CHANGED_MODE）也能继续用
    }

    const CmdLine cli = parse_cmdline();

    if (!cli.shot_dir.empty()) {
        attach_parent_console();
        const int rc = run_screenshot_mode(to_utf8(cli.data_dir), to_utf8(cli.shot_dir),
                                           cli.width, cli.height);
        App::get().shutdown();
        return rc;
    }

    App& app = App::get();
    std::wstring err;
    if (!app.init(to_utf8(cli.data_dir), &err)) {
        MessageBoxW(nullptr, err.c_str(), L"PenHu Ledger", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!Shell::get().init(hinst, to_utf8(cli.data_dir), &err)) {
        MessageBoxW(nullptr, err.c_str(), L"PenHu Ledger", MB_OK | MB_ICONERROR);
        app.shutdown();
        return 1;
    }

    if (!cli.diag_user.empty()) {
        Shell::get().configure_diag(to_utf8(cli.diag_user), to_utf8(cli.diag_pass),
                                    to_utf8(cli.diag_shot), to_utf8(cli.diag_scan_dir));
    }

    const int rc = Shell::get().run();
    app.shutdown();
    return rc;}
