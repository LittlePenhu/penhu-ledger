// =============================================================================
//  native/app/shell.cpp —— Win32 窗口壳：初始化、消息泵、消息分发
//  （窗口帧/动画在 shell_window.cpp；诊断自动化在 shell_diag.cpp）
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

constexpr wchar_t kWindowClass[] = L"PenHuLedgerNativeWindow";
constexpr wchar_t kWindowTitle[] = L"PenHu Ledger · 每日花销";
constexpr UINT    kMsgRefresh = WM_APP + 1;
constexpr UINT_PTR kTimerTick = 1;

/// 未捕获 C++ 异常的统一处理：写日志 + 提示用户。
///
/// 为什么必须有它：GUI 程序里未捕获异常 = std::terminate = abort()，
/// 进程直接消失、日志一行都不留 —— 用户只看到「闪退」，
/// 连「是哪个功能崩的」都查不出来。（2026-09-21 的识图闪退就是这样：
/// 只能先解析 WER 崩溃转储，再重建一份带链接映射表的 exe 反推符号，
/// 才定位到是一个嵌套 lock_guard。有这段代码的话，当时日志里就会直接写出
/// 「resource deadlock would occur」。）
///
/// 提示文本用纯 Win32 转换而不是 ui::to_wide —— 这个函数本身是异常处理路径，
/// 不能再依赖可能抛异常的代码，否则会在 catch 里再抛一次直接 terminate。
void report_uncaught(const char* where, const std::exception* e) {
    std::string msg = std::string("未捕获异常 @ ") + where;
    if (e != nullptr) {
        msg += "：";
        msg += e->what();
    }
    Logger::default_logger().error(msg);

    if (g_diag_mode) return;   // 无人值守：只留日志，不弹等待点击的框

    wchar_t wbuf[1024] = {0};
    MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wbuf, 1023);
    MessageBoxW(nullptr, wbuf, L"PenHu 遇到内部错误（详情已写入日志）",
                MB_OK | MB_ICONERROR);
}

/// 跟随系统的浅色/深色偏好。M3 的 auto 主题在原生里的等价物。
bool system_prefers_dark() {
    DWORD value = 1;   // 默认浅色
    DWORD size = sizeof(value);
    const LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (st != ERROR_SUCCESS) return false;
    return value == 0;
}

}  // namespace

Shell& Shell::get() {
    static Shell instance;
    return instance;
}

float Shell::dpi_scale() const {
    const UINT dpi = hwnd_ != nullptr ? GetDpiForWindow(hwnd_) : 96u;
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

bool Shell::init(HINSTANCE hinst, const std::string& data_dir, std::wstring* err) {
    if (!renderer_.init()) {
        if (err) *err = L"图形子系统初始化失败（Direct2D / DirectWrite / WIC）";
        return false;
    }

    App& app = App::get();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // 不带 CS_HREDRAW / CS_VREDRAW：它们让每次尺寸变化都把**整个窗口**
    // 标记失效。我们本来就在 WM_SIZE 里当场重绘整帧，这套协助只会让系统
    // 在我们画完之后又按失效区走一遍合成 —— 缩放和最大化动画时的闪烁源。
    wc.style = 0;
    wc.lpfnWndProc = &Shell::wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // exe 资源里的图标（resources/app.rc 的 IDI 1）。任务栏、Alt+Tab、
    // 资源管理器都用它；顶栏左侧那个 logo 是同一张图的 PNG 版本。
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = nullptr;   // 自绘，不要系统背景刷（否则 resize 时闪白）
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        if (err) *err = L"窗口类注册失败";
        return false;
    }

    // 窗口尺寸按「工作区」（去掉任务栏的可用桌面）自适应。
    // 写死 1240x860 在小屏（1366x768 之类）上会超出屏幕，底部导航被切掉，
    // 而底部导航正是切页的唯一入口 —— 那等于把应用变残。
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) == FALSE) {
        work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int avail_w = work.right - work.left;
    const int avail_h = work.bottom - work.top;

    // 窗口尺寸按**逻辑单位**设计，再乘系统 DPI 换算成物理像素。
    //
    // 直接写 1240x860 物理像素在 150% 缩放的屏幕上只有 827x573 逻辑单位 ——
    // 逻辑宽度不到 820 就要掉进 Compact 档：侧边栏占掉一大块、内容区只剩六百多，
    // 双栏排不开、分类格子从 6 列掉到 3 列。用户看到的是「窗口莫名其妙很小」，
    // 而真正的原因是尺寸用了物理像素、判断用了逻辑单位，两把尺子。
    const UINT sys_dpi = GetDpiForSystem();
    const float s0 = sys_dpi > 0 ? static_cast<float>(sys_dpi) / 96.0f : 1.0f;
    const int w = std::min(static_cast<int>(1240.0f * s0),
                           std::max(static_cast<int>(900.0f * s0), avail_w - 80));
    const int h = std::min(static_cast<int>(860.0f * s0),
                           std::max(static_cast<int>(620.0f * s0), avail_h - 60));
    const int sx = work.left + (avail_w - w) / 2;
    const int sy = work.top + (avail_h - h) / 2;

    // 无边框窗口的样式。这段很容易写错，逐条说明：
    //
    //   · **必须去掉 WS_CAPTION**。它唯一的用途就是「让系统画那条标题栏」——
    //     只把 WM_NCCALCSIZE 返回 0 是不够的：那只把非客户区的高度算成 0，
    //     系统照样按 WS_CAPTION 去画标题栏。实测后果是窗口顶部**同时**出现
    //     系统标题栏和自绘顶栏，两条并排。
    //   · WS_THICKFRAME 必须留：窗口阴影、可缩放边框、Aero Snap 都靠它。
    //   · WS_SYSMENU 必须留：右键顶栏要能弹出系统菜单（还原/移动/大小/关闭）。
    //   · MINIMIZEBOX / MAXIMIZEBOX：少了 ShowWindow(SW_MAXIMIZE) 不起作用。
    //
    // WS_CAPTION 必须留着。它不是用来画标题栏的 —— WM_NCCALCSIZE 把
    // 非客户区算成 0 之后系统根本没地方画 —— 而是让 Windows **不要**把
    // 本窗口判定为「全屏窗口」：无边框窗口最大化时矩形会外扩到显示器之外，
    // 盖满屏幕的系统待遇是任务栏被自动隐藏，露出来的就是没人画的外扩环
    // （用户报告的「任务栏区域被涂黑」）。带 caption 的窗口再大也享受不到
    // 这个待遇，任务栏始终可见。这是 Chromium / Windows Terminal 的标准做法。
    const DWORD kFrameStyle = WS_CAPTION | WS_POPUP | WS_THICKFRAME |
                              WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

    // WS_EX_COMPOSITED：把窗口的绘制交给 DWM 的后备缓冲做双缓冲。
    //
    // 为什么需要它：我们是「渲染到内存位图 + SetDIBitsToDevice 直写窗口表面」。
    // 直写意味着**绘制过程本身是可见的** —— 一次 2560x1528x4 ≈ 15MB 的写入
    // 跨过了 DWM 的合帧时刻，屏幕上就会出现上半张旧帧、下半张新帧的撕裂；
    // 拖拽缩放和窗口动画期间每帧都在重画，看到的正是「闪」。
    // COMPOSITED 之后写入先落在后备缓冲，DWM 等我们这一帧画完再整体翻页，
    // 露底与撕裂都消失。
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_COMPOSITED, kWindowClass, kWindowTitle, kFrameStyle,
        sx > 0 ? sx : 0, sy > 0 ? sy : 0, w, h,
        nullptr, nullptr, hinst, nullptr);
    if (hwnd_ == nullptr) {
        if (err) *err = L"窗口创建失败";
        return false;
    }

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 每显示器 DPI 感知（v2）已在 main 里设置；这里确认 DPI 变化会收到消息
    const float scale = dpi_scale();
    if (!renderer_.attach_window(hwnd_, scale)) {
        if (err) *err = L"渲染目标绑定窗口失败";
        return false;
    }
    dark_ = system_prefers_dark();
    theme_ = dark_ ? ui::Theme::dark() : ui::Theme::light();
    app.dark = dark_;

    // 无边框窗口的 DWM 设置（圆角 + 系统边框色）。必须在 theme_ 定下来之后，
    // 因为边框色是跟着主题走的。
    apply_window_frame();

    // App 请求重绘 → 发消息给窗口（可跨线程）
    app.set_repaint([this]() {
        if (hwnd_ != nullptr) PostMessageW(hwnd_, kMsgRefresh, 0, 0);
    });

    // 顶栏上的窗口按钮接到本窗口。**必须在 assemble 之前** ——
    // assemble 会把回调绑到 TitleBar 上，晚设置就没人接了。
    tree_.request_minimize = [this]() { ShowWindow(hwnd_, SW_MINIMIZE); };
    tree_.request_toggle_maximize = [this]() { toggle_maximize(); };
    tree_.request_close = [this]() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); };

    tree_.assemble(app);

    // 顶栏 logo：从 exe 资源（RCDATA 2，与 ico 同源的 PNG）加载。
    // 找不到资源就静默跳过 —— 顶栏少一个 logo 不该影响可用性。
    {
        HRSRC rc = FindResourceW(hinst, MAKEINTRESOURCEW(2), RT_RCDATA);
        if (rc != nullptr) {
            HGLOBAL h = LoadResource(hinst, rc);
            if (h != nullptr) {
                const void* p = LockResource(h);
                const DWORD n = SizeofResource(hinst, rc);
                if (p != nullptr && n > 0) {
                    tree_.title->set_logo_png(std::vector<uint8_t>(
                        static_cast<const uint8_t*>(p),
                        static_cast<const uint8_t*>(p) + n));
                }
            }
        }
    }

    // 首帧之前先定宽度档：多栏还是单栏是在**构建页面时**决定的，
    // 所以必须赶在第一次 rebuild_pages 之前算出来。
    {
        const float w = renderer_.logical_size().w;
        app.width_class = (w < 820.0f) ? WidthClass::Compact
                                       : (w < 1180.0f ? WidthClass::Medium
                                                      : WidthClass::Expanded);
    }
    tree_.rebuild_pages(app);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTimerTick, 16, nullptr);
    return true;
}

int Shell::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Shell::request_repaint() {
    if (hwnd_ != nullptr) PostMessageW(hwnd_, kMsgRefresh, 0, 0);
}

void Shell::set_focus(ui::Widget* w) {
    if (focus_ == w) return;
    if (focus_ != nullptr) {
        focus_->focused = false;
        focus_->on_focus_lost();
    }
    focus_ = w;
    if (focus_ != nullptr) focus_->focused = true;
}

void Shell::cycle_focus() {
    std::vector<ui::Widget*> list;
    tree_.root.collect_focusables(list);
    if (list.empty()) return;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == focus_) {
            set_focus(list[(i + 1) % list.size()]);
            return;
        }
    }
    set_focus(list.front());
}

bool Shell::ctrl_pressed() const {
    // 两个来源取或：自己维护的（能看见注入的消息）+ 系统的真实状态（能兜住漏掉的 KEYUP）
    return ctrl_down_ || ui::key_down(VK_CONTROL);
}

bool Shell::dispatch_edit_shortcut(unsigned vk) {
    if (focus_ == nullptr) return false;
    ui::EditCmd cmd;
    switch (vk) {
        case 'C': cmd = ui::EditCmd::Copy;      break;
        case 'X': cmd = ui::EditCmd::Cut;       break;
        case 'V': cmd = ui::EditCmd::Paste;     break;
        case 'A': cmd = ui::EditCmd::SelectAll; break;
        default:  return false;
    }
    return focus_->on_edit_command(cmd);
}

void Shell::handle_ime(HWND hwnd, LPARAM lp) {
    if ((lp & GCS_RESULTSTR) == 0) return;
    HIMC imc = ImmGetContext(hwnd);
    if (imc == nullptr) return;

    const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
    if (bytes > 0) {
        std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
        ImmGetCompositionStringW(imc, GCS_RESULTSTR, text.data(), bytes);
        if (focus_ != nullptr) {
            for (wchar_t c : text) focus_->on_text(c);
        }
    }
    ImmReleaseContext(hwnd, imc);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Shell::relayout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // 动画/拖拽进行中不允许「缩小时回收容量」：回收重建落在运动的某一帧上
    // 就是一次可见的卡顿。等运动结束后的自然尺寸变化再收（那是一次性的，
    // 也不在任何运动时间轴上）。
    renderer_.resize(rc.right - rc.left, rc.bottom - rc.top,
                     !win_anim_.active && !resizing_);
}

void Shell::rebuild_pages_safely() {
    hovered_ = nullptr;
    pressed_ = nullptr;
    set_focus(nullptr);
    tree_.rebuild_pages(App::get());
}

void Shell::paint() {
    ++paint_count_;
    App& app = App::get();
    tree_.sync(app, GetTickCount64());

    const ui::Size size = renderer_.logical_size();
    if (!renderer_.begin_frame(theme_.palette.surface)) return;

    tree_.root.layout({0.0f, 0.0f, size.w, size.h}, renderer_);
    tree_.root.paint(renderer_, theme_);

    if (!renderer_.end_frame()) {
        // 设备丢失（切换显示器、驱动重置）：重建渲染目标，下一帧会重画
        renderer_.attach_window(hwnd_, dpi_scale());
        return;
    }
    // 软件渲染到离屏位图，画完再把这一帧贴到窗口上
    renderer_.present();
}

LRESULT CALLBACK Shell::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
    // 兜住消息处理里的未捕获异常。异常穿过 Win32 的消息分发是未定义行为，
    // 而且会一路走到 std::terminate —— 表现就是「点一下整个程序消失」。
    // 在这里接住：写日志、弹一次提示，进程继续活着（用户至少知道发生了什么）。
    try {
        return self->handle(hwnd, msg, wp, lp);
    } catch (const std::exception& e) {
        report_uncaught("窗口消息处理", &e);
    } catch (...) {
        report_uncaught("窗口消息处理", nullptr);
    }
    return 0;
}

LRESULT Shell::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = App::get();

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // 全部自绘，避免清背景导致的闪烁

        // ---- 无边框窗口：把系统标题栏「吃掉」 ----
        // 做法不是去掉 WS_CAPTION，而是保留正常的窗口样式、只把非客户区的尺寸
        // 算成 0。保留样式换来三件免费的事：窗口阴影、Aero Snap、右键系统菜单 ——
        // 这些在纯 WS_POPUP 窗口上都得自己写一遍。
        case WM_NCCALCSIZE: {
            if (wp == FALSE) break;   // 不处理这种调用，走默认流程
            // 最大化时系统会按「有边框」把窗口矩形外扩到屏幕外，
            // 客户区必须改成**工作区**，否则内容底部会被任务栏盖住。
            // 全屏则相反 —— 它的定义就是连任务栏一起盖掉。
            if (fullscreen_ || is_maximized()) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    // 注意坐标系：NCCALCSIZE 里 rgrc[0] 用的是**屏幕坐标**
                    p->rgrc[0] = fullscreen_ ? mi.rcMonitor : mi.rcWork;
                }
            }
            return 0;   // 其余情况：客户区 = 整个窗口矩形
        }

        // 顶栏空白处返回 HTCAPTION —— 拖动窗口、双击最大化、拖到边缘吸附、
        // 右键出系统菜单，全部交给系统。自己用 SetWindowPos 实现拖动会
        // 把这些全丢掉，而且拖动时不跟手。
        case WM_NCHITTEST: {
            const LRESULT base = DefWindowProcW(hwnd, WM_NCHITTEST, wp, lp);
            // 边框方向（HTLEFT/HTTOP/…）直接采用，那是系统算好的缩放热区。
            // 但**最大化和全屏都没有**：最大化时那圈边缘就是屏幕边缘（拖不动），
            // 全屏的定义就是铺满整个显示器、不留边框。
            // 留着热区的后果很具体：光标变成缩放箭头，却怎么拉都拉不动。
            const bool no_resize = is_maximized() || fullscreen_;
            if (base != HTCLIENT) return no_resize ? HTCLIENT : base;
            if (no_resize) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};   // NCHITTEST 给的是屏幕坐标
            ScreenToClient(hwnd, &pt);
            const ui::Point p = to_dip(pt.x, pt.y);
            if (tree_.title != nullptr && tree_.title->hit_for_drag(p)) return HTCAPTION;
            return HTCLIENT;
        }

        // 最小窗口尺寸。再小双栏会挤成一团，顶栏右侧的按钮也会开始重叠。
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const float s = dpi_scale();
            mmi->ptMinTrackSize.x = static_cast<LONG>(760.0f * s);
            mmi->ptMinTrackSize.y = static_cast<LONG>(560.0f * s);
            return 0;
        }

        // 无边框窗口激活/失活时不要让系统去补画非客户区
        // （那会闪出一条系统色的横带）
        case WM_NCACTIVATE:
            return TRUE;

        // 系统菜单、Win+↑ / Win+↓、任务栏图标的右键菜单都会走到这里。
        // 全屏时这两条命令的语义必须改写成「退出全屏」—— 否则会出现
        // 「窗口缩回了工作区，但任务栏还是被盖着」这种一半一半的状态。
        case WM_SYSCOMMAND: {
            const int cmd = static_cast<int>(wp) & 0xFFF0;
            if (fullscreen_) {
                if (cmd == SC_RESTORE || cmd == SC_MAXIMIZE) {
                    toggle_fullscreen();
                    return 0;
                }
            } else if (cmd == SC_MAXIMIZE) {
                // Win+↑、双击顶栏、任务栏右键菜单的「最大化」全走这里。
                // 必须拦下来自己实现：放行的话系统会走 SW_MAXIMIZE，
                // 窗口外扩进 zoomed 态 —— 任务栏隐藏、外扩环露黑条、
                // 还要和我们的动画机制打架。
                toggle_maximize();
                return 0;
            } else if (cmd == SC_RESTORE && manual_max_) {
                toggle_maximize();
                return 0;
            }
            break;   // 其余交给默认处理：最小化 / 关闭都靠它
        }

        // 双击顶栏。系统对 HTCAPTION 的默认行为是切换最大化，
        // 但全屏是另一种状态 —— 那时双击应该是退出全屏。
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTCAPTION && fullscreen_) {
                toggle_fullscreen();
                return 0;
            }
            break;

        // ---- 拖拽窗口（缩放或移动）----
        // 系统在这两条消息之间会进入一个模态消息循环，期间的 WM_SIZE
        // 是「鼠标每动一下就来一次」。用它把重建页面这类重活推迟到松手后：
        // 拖拽途中要的是跟手，不是每一帧都把布局算到最优。
        case WM_ENTERSIZEMOVE:
            resizing_ = true;
            return 0;

        case WM_EXITSIZEMOVE:
            resizing_ = false;
            if (pending_rebuild_) {
                pending_rebuild_ = false;
                rebuild_pages_safely();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SIZE: {
            // 最小化时不需要画，也不该去动渲染目标（客户区是 0x0）。
            if (wp == SIZE_MINIMIZED) return 0;

            relayout();
            sync_window_state();

            // 窗口宽度跨过档位时重建页面：多栏/单栏是按档位在构建期决定的，
            // 不重建的话拖动窗口大小不会改变布局。
            // 注意走 rebuild_pages_safely：直接 rebuild 会把 hovered_ 等指针
            // 留在已销毁的子树上 —— 缩放时鼠标恰好停在界面上就会踩到。
            {
                App& app = App::get();
                const float w = renderer_.logical_size().w;
                const WidthClass want = (w < 820.0f) ? WidthClass::Compact
                                                     : (w < 1180.0f ? WidthClass::Medium
                                                                    : WidthClass::Expanded);
                if (want != app.width_class) {
                    app.width_class = want;
                    if (win_anim_.active || resizing_) {
                        // 动画 / 拖拽途中尺寸每帧都在变，可能反复跨档。
                        // 每帧重建整棵页面树太重（一次上百个组件），
                        // 记下来，等结束补一次就够了。
                        pending_rebuild_ = true;
                    } else {
                        rebuild_pages_safely();
                    }
                }
            }

            // ★ 这里**同步画一帧**，不能只 InvalidateRect。
            //
            // 失效区要等到消息队列空了才触发 WM_PAINT，而拖拽缩放时
            // 窗口已经被拖到下一个尺寸了 —— 中间那几帧处于「窗口已经变大、
            // 但还没画」的状态，露出来的就是窗口背景，即肉眼看到的闪白与抖动。
            // 在 WM_SIZE 里当场画完，窗口尺寸和画面就永远是一致的。
            if (renderer_.has_target()) {
                paint();
                // 已经画完了，把失效区清掉，免得紧接着又来一次 WM_PAINT
                // 把同一帧画两遍（缩放时这等于每步多做一次全屏重绘）。
                ValidateRect(hwnd, nullptr);
            } else {
                // 窗口刚创建、渲染目标还没 attach 好，交给后面的 WM_PAINT
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            paint();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            // NOCOPYBITS：旧像素是按旧 DPI 画的，拷贝/拉伸到新几何就是错位帧。
            // 紧接着会 detach/attach 重画，中间态不值得让系统描出来。
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            const float scale = dpi_scale();
            renderer_.detach();
            renderer_.attach_window(hwnd_, scale);
            relayout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const ui::Point p = to_dip(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

            // 按住左键时，事件捕获给「按下时消费了它」的那个组件。
            // 拖动滚动条就是这么工作的：鼠标一旦移出滑块（甚至移出窗口），
            // 没有捕获就会在移出的瞬间断掉 —— 症状是「拖着拖着不动了」。
            if (pressed_ != nullptr && (wp & MK_LBUTTON) != 0) {
                if (pressed_->on_pointer_move(p, pressed_->bounds().contains(p))) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            ui::Widget* hit = tree_.root.hit_test(p);
            if (hit != hovered_) {
                if (hovered_ != nullptr) hovered_->on_pointer_move(p, false);
                hovered_ = hit;
                if (hit != nullptr) hit->on_pointer_move(p, true);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (hit != nullptr) {
                // 命中的组件没变，但**组件内部的**悬停目标可能变了：
                // 鼠标从顶栏的「最小化」滑到「关闭」，命中始终是 TitleBar，
                // 可高亮该换一个按钮了。所以这里也要看返回值 ——
                // 它表示「我需要重绘」，而不是「我消费了这个事件」。
                if (hit->on_pointer_move(p, true)) InvalidateRect(hwnd, nullptr, FALSE);
            }
            // 手型光标：可点组件上用 IDC_HAND。这在桌面端是「这里能点」的第一提示。
            const bool clickable = hit != nullptr && hit->focusable;
            SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            const ui::Point p = to_dip(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            // 冒泡找消费者：可点组件消费它就等于「按在按钮上」，
            // 没人消费时落在滚动容器的滚动条上也能被它接住。
            ui::Widget* consumer = bubble_down(tree_.root.hit_test(p), p);
            pressed_ = consumer;
            if (consumer != nullptr && consumer->focusable) {
                set_focus(consumer);
            } else if (consumer == nullptr || !consumer->preserve_focus_on_press) {
                // 点空白处收起焦点（桌面端惯例）；但拖滚动条时保持不动，
                // 否则正在输入的备注框会失焦、输入法未提交的内容会丢。
                set_focus(nullptr);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            ReleaseCapture();
            const ui::Point p = to_dip(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            ui::Widget* hit = tree_.root.hit_test(p);
            if (pressed_ != nullptr) {
                // 按下与抬起必须在同一个组件上才算一次点击；
                // 否则给一个界外坐标，让组件自己取消（组件的 on_pointer_up
                // 会检查坐标是否落在自己范围内）。
                // 拖动滚动条的组件对界外坐标是安全的：它的 on_pointer_up
                // 只负责结束拖拽状态，不做命中判断。
                pressed_->on_pointer_up(hit == pressed_ ? p : ui::Point{-100000.0f, -100000.0f});
            }
            pressed_ = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const ui::Point p = to_dip(pt.x, pt.y);
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                                static_cast<float>(WHEEL_DELTA);
            bubble_wheel(tree_.root.hit_test(p), p, delta);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            // F11：全屏（占满整个显示器，含任务栏）。
            // 它和「最大化」是两回事 —— 最大化只占工作区，不碰任务栏。
            if (wp == VK_F11) {
                toggle_fullscreen();
                return 0;
            }
            // Esc：退出全屏。全屏最容易让人找不到出口，
            // 除了 F11 和顶栏按钮，再给一个最直觉的键。
            if (wp == VK_ESCAPE && fullscreen_) {
                toggle_fullscreen();
                return 0;
            }
            if (wp == VK_CONTROL) {
                ctrl_down_ = true;
                return 0;
            }
            if (wp == VK_TAB) {
                cycle_focus();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Ctrl 组合键优先走编辑命令（复制/剪切/粘贴/全选），
            // 拦下之后就不再往下传 —— 否则 V 会被当成普通字符输入。
            if (ctrl_pressed() && dispatch_edit_shortcut(static_cast<unsigned>(wp))) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (focus_ != nullptr) {
                focus_->on_key(static_cast<unsigned>(wp), true);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYUP:
            if (wp == VK_CONTROL) ctrl_down_ = false;
            if (focus_ != nullptr) focus_->on_key(static_cast<unsigned>(wp), false);
            return 0;

        case WM_CHAR: {
            // 输入法已经在 WM_IME_COMPOSITION 里提交过结果字符串，
            // 这里只处理普通按键产生的字符，避免中文被写入两次。
            const wchar_t ch = static_cast<wchar_t>(wp);
            // Ctrl 组合键产生的字符不进输入框。标准键盘下 Ctrl+V 产生 0x16
            // （已被下面的 ch >= 32 拦掉），但消息注入、某些 IME、非常规键盘
            // 布局下 Ctrl+字母可能产生字面字符 —— 那条路必须堵死，否则粘贴
            // 内容后面会跟着一个多余字符（诊断注入就稳定复现了这种现象）。
            // AltGr 例外：它等于 Ctrl+Alt，产生的是合法字符（如德语布局的 @）。
            const bool altgr = ctrl_pressed() && ui::key_down(VK_MENU);
            if (ctrl_pressed() && !altgr) return 0;
            if (focus_ != nullptr && ch >= 32) {
                focus_->on_text(ch);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_IME_COMPOSITION:
            handle_ime(hwnd, static_cast<LPARAM>(lp));
            return 0;

        case WM_TIMER:
            if (wp == kTimerTick) {
                const uint64_t now = GetTickCount64();
                // 真实帧间隔。用真实值而不是常量 16ms：系统忙碌时定时器会漂，
                // 动画按真实 dt 走才不会忽快忽慢。
                float dt_ms = 16.0f;
                if (last_tick_ms_ != 0 && now > last_tick_ms_) {
                    dt_ms = static_cast<float>(now - last_tick_ms_);
                }
                last_tick_ms_ = now;
                // 时钟被改或者系统休眠后回来，dt 会大得离谱 —— 夹一下，
                // 否则动画会一步跳完（甚至算出负数）
                dt_ms = std::min(dt_ms, 100.0f);

                // 光标闪烁：每 500ms 翻转。按时间判断而不是「第 N 次 tick」——
                // 帧率改成 16ms 后，原来那个 tick 计数会变成 80ms 一闪。
                if (now - last_caret_ms_ >= 500) {
                    last_caret_ms_ = now;
                    if (focus_ != nullptr) {
                        if (auto* tf = dynamic_cast<ui::TextField*>(focus_)) tf->tick_caret();
                    }
                }

                // 推进动画。
                // 窗口矩形动画（最大化/还原/全屏）优先：它改的是窗口本身，
                // 组件状态不因此变化，所以两者可以并存。
                const bool window_animating = tick_window_anim(now);
                // 返回 true 表示这一帧画面有变化
                const bool animating = window_animating || tree_.root.tick(dt_ms);

                if (animating) {
                    InvalidateRect(hwnd, nullptr, FALSE);   // 滚动等动画
                } else if (app.busy) {
                    InvalidateRect(hwnd, nullptr, FALSE);   // 驱动进度点号动画
                } else if (app.toast_alive(now)) {
                    InvalidateRect(hwnd, nullptr, FALSE);   // 让提示条在过期时消失
                }

                // ---- 诊断模式：自动登录 → 渲染一帧 → 存图 → 退出 ----
                // 走的是和真人完全相同的路径（异步登录 + pump + 重建 + 绘制 + present），
                // 所以它能复现「登录后闪退」这类只在真实窗口里出现的问题。
                tick_diag(hwnd, dt_ms);
            }
            return 0;

        case kMsgRefresh:
            app.pump();
            // 重建会销毁 page_host 下的整棵子树，而 hovered_ / pressed_ / focus_
            // 都还指着那些组件 —— 必须先清掉，否则下一次鼠标移动或按键就会
            // 解引用已释放的内存。
            //
            // 这正是「点一下就闪退」的原因：点击 → 组件的 on_click 里请求重建
            // （消息是异步的，所以按下与抬起本身没问题）→ 树被换掉 →
            // 你随手动一下鼠标，WM_MOUSEMOVE 去通知那个已经不存在的组件。
            // 顺序很重要：先清指针，再重建。
            // （这段逻辑抽成了 rebuild_pages_safely()，这里只是要保住滚动位置，
            //  所以下面手动走一遍：先清指针 → 记滚动 → 重建 → 还滚动。）

            // 重建前记下滚动位置，重建后还回去。
            //
            // 不还的话，**每一次 rebuild_ui() 都会把页面弹回顶部** ——
            // 而 rebuild_ui 会被每一次点击调用（点分类、切方向、保存、删记录…）。
            // 于是「滚到列表中间点一个分类」，页面立刻跳回顶端，用户得重新滚。
            // （这个 bug 是在给提示条做回归测试时暴露的：断言 offset 变化，
            // 结果发现它在重建那一步被清零了。）
            {
                const bool same_page = (app.screen == last_screen_);
                float saved = 0.0f;
                if (ui::ScrollView* sv = find_scrollview(&tree_.root)) {
                    saved = sv->target_offset;
                }
                rebuild_pages_safely();
                // 只在同页时恢复：切页本来就该从头看。
                // 页面自己主动滚动过（比如切进编辑态时 ensure_visible 定位到那笔记录）
                // 就尊重它，不要用旧位置覆盖。
                if (same_page && saved > 0.0f) {
                    if (ui::ScrollView* sv = find_scrollview(&tree_.root)) {
                        if (sv->target_offset <= 0.0f) sv->scroll_to(saved, true);
                    }
                }
                last_screen_ = app.screen;
            }

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerTick);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
//  截图模式（离屏）
// -----------------------------------------------------------------------------

int run_screenshot_mode(const std::string& data_dir, const std::string& out_dir,
                        int width, int height) {
    App& app = App::get();
    std::wstring err;
    if (!app.init(data_dir, &err)) {
        Logger::default_logger().error("截图模式初始化失败：" + ui::to_utf8(err));
        return 1;
    }

    ui::Renderer rr;
    if (!rr.init() || !rr.attach_offscreen(width, height, 1.0f)) {
        Logger::default_logger().error("截图模式：离屏渲染目标创建失败");
        return 1;
    }

    // 截图宽度也要参与档位判定：多栏/单栏是**构建期**决定的，
    // 不设的话所有截图都会用窗口模式的默认档，等于没验到多栏。
    app.width_class = (width < 820) ? WidthClass::Compact
                                    : (width < 1180 ? WidthClass::Medium
                                                    : WidthClass::Expanded);
    // 显式给个展开态：截图模式不跑消息循环（不调 pump），
    // 所以不会去读账号里存的折叠偏好 —— 那样截图内容就取决于
    // 「这个账号上次点了什么」，同一份代码截出来的图会飘。
    app.nav_expanded = true;

    AppTree tree;
    tree.assemble(app);

    const ui::Theme light = ui::Theme::light();
    const ui::Theme dark = ui::Theme::dark();

    // rebuild=false 用于「同一页面的第二张截图」：先截首屏，再滚动一下截下半页。
    // 带 rebuild 的话会把刚设好的滚动位置冲掉（重建 = 新 ScrollView，offset 归零）。
    auto shoot = [&](const ui::Theme& th, const std::string& name,
                     bool rebuild = true) -> bool {
        if (rebuild) tree.rebuild_pages(app);
        tree.sync(app, 1000);   // 固定时间戳，避免截图内容随运行时刻变化
        // 推一次动画让它一步到位：宽度/开关这类过渡状态是逐帧插值的，
        // 而截图模式没有消息循环（不跑 tick）—— 不推的话截到的是过渡中间态，
        // 「折叠后应该多宽」这种结论就完全对不上了。
        tree.root.tick(500.0f);
        if (!rr.begin_frame(th.palette.surface)) return false;
        tree.root.layout({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)}, rr);
        tree.root.paint(rr, th);
        if (!rr.end_frame()) return false;
        const std::string path = out_dir + "\\" + name + ".png";
        if (!rr.save_png(path)) {
            Logger::default_logger().error("截图保存失败：" + path);
            return false;
        }
        Logger::default_logger().info("已截图 " + path);
        return true;
    };

    // ---- 1) 登录页（未登录态）----
    app.screen = Screen::Login;
    if (!shoot(light, "01_login")) return 1;

    // ---- 2) 登录 + 灌数据 ----
    if (!prepare_demo_account(app)) return 1;

    app.go(Screen::Entry);
    app.entry_amount = L"38.50";
    app.entry_note = L"兰州拉面";
    if (!shoot(light, "02_entry")) return 1;
    app.entry_amount.clear();
    app.entry_note.clear();

    app.go(Screen::Records);
    if (!shoot(light, "02b_records")) return 1;

    app.go(Screen::Stats);
    if (!shoot(light, "03_stats")) return 1;

    app.go(Screen::Report);
    if (!shoot(light, "04_report")) return 1;

    // 截图识别页：造三条不同状态的记录，让「列表 + 详情」两块的排版都能被核对
    app.go(Screen::Scan);
    app.scan_items.clear();
    {
        ScanItem done;
        done.label = L"wxpay_20260920_1230.png";
        done.dims = L"1080×2400";
        done.state = ScanItem::State::Done;
        done.draft.ready = true;
        done.draft.amount_minor = 3850;
        done.draft.direction = 0;
        done.draft.category_name = "餐饮";
        done.draft.category_id = app.category_id_of_name("餐饮");
        done.draft.merchant = "兰州拉面（万达店）";
        done.draft.date_iso = Date::today().to_string();
        done.draft.note = "微信支付 · 午餐";
        done.draft.confidence = 0.93;
        done.draft.method = "cloud-openai / 视觉模型";
        app.scan_items.push_back(std::move(done));

        ScanItem queued;
        queued.label = L"alipay_20260921_0815.png";
        queued.dims = L"1170×2532";
        app.scan_items.push_back(std::move(queued));

        ScanItem failed;
        failed.label = L"bank_notice_blur.jpg";
        failed.dims = L"828×1792";
        failed.state = ScanItem::State::Failed;
        failed.fail_reason = "金额区域太模糊，无法确认具体数字（不会用别的数字顶替）";
        app.scan_items.push_back(std::move(failed));
    }
    app.scan_selected = 0;
    if (!shoot(light, "05_scan")) return 1;

    app.go(Screen::Settings);
    if (!shoot(light, "06_settings")) return 1;
    // 设置页很长，再截一张下半部分（截图目录 / 分类管理那几节）——
    // 只看首屏的话，新加的设置项根本不出现在验证图里。
    if (ui::ScrollView* sv = find_scrollview(&tree.root)) {
        sv->scroll_to(520.0f, true);
        if (!shoot(light, "06b_settings_lower", /*rebuild=*/false)) return 1;
    }

    // 顺带截一张折叠态：验证「收起后只剩图标」这条路径
    app.nav_expanded = false;
    if (!shoot(light, "09_rail_collapsed")) return 1;
    app.nav_expanded = true;

    // ---- 3) 深色主题抽查两张 ----
    // **必须设 app.dark**：页面构建时会把 palette 色值固化进组件成员
    // （比如信息行的次要文字色），只把 shoot 的绘制参数换成深色是不够的 ——
    // 那样截出来的深色图里，构建期固化的颜色还是浅色版的值，
    // 深底上画浅色版的次要灰，对比度不够，文字几乎看不见。
    // 这个 bug 潜伏了很久：多数组件从 paint 参数拿色所以一直看着正常。
    app.dark = true;
    app.go(Screen::Entry);
    if (!shoot(dark, "07_entry_dark")) return 1;
    app.go(Screen::Stats);
    if (!shoot(dark, "08_stats_dark")) return 1;
    app.dark = false;

    Logger::default_logger().info("截图模式完成，输出目录 " + out_dir);
    return 0;
}

}  // namespace penhu::native
