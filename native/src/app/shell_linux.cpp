// =============================================================================
//  native/app/shell_linux.cpp
//  Linux 侧的窗口外壳：Wayland（xdg-shell）+ wl_shm + xkbcommon。
//
//  和 Windows 那份（shell.cpp）比，这里少了大约一千行，原因不是「Linux 简单」，
//  而是三件事在 Wayland 里**不是客户端该操心的**：
//
//    · 窗口的位置和尺寸 —— 由合成器决定，客户端只能「协商」。所以没有
//      「按工作区算一个位置」「把窗口摆到 rcWork」「最大化时矩形外扩到屏幕外」
//      这一整类代码，也就没有 Windows 那边为了让任务栏不被藏起来而写的
//      手动最大化 + WS_CAPTION 补丁（那一串全是为了绕开 Win32 的固有行为）。
//    · 窗口边框和阴影 —— 由合成器画（服务端装饰）或客户端自绘（CSD）。
//      我们用 CSD（自定义顶栏本来就是这个应用的形态），不需要处理系统标题栏。
//    · 修饰键的全局状态 —— Wayland 不给客户端这个能力，所以 key_down() 改成
//      由这里喂状态（见 ui/platform_input_linux.cpp 的说明）。
//
//  输入分发的语义和 Windows 完全一致：冒泡、焦点、Ctrl 组合键、滚动条拖动。
//  那些代码在 shell_common.cpp 与 ui/ 里，两个平台共用一份 —— 不是「重新实现了一遍」。
// =============================================================================

#include "app/shell.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "app/platform_fs.hpp"
#include "app/shell_common.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/process.hpp"
#include "ui/convert.hpp"
#include "ui/keys.hpp"
#include "ui/platform_input.hpp"
#include "xdg-shell-client-protocol.h"

namespace penhu::native {

// -----------------------------------------------------------------------------
//  未捕获异常 / 系统主题
// -----------------------------------------------------------------------------

/// 未捕获 C++ 异常的统一处理。
///
/// Windows 那份会弹 MessageBox；Linux 这边**只写日志**，因为 Wayland 客户端
/// 没有「系统级弹框」这种东西 —— 要做只能自己画一个窗口，而那本身就是
/// 「绘制已经出问题了」的时候，再依赖绘制去报错是循环依赖。
/// 所以这里把信息写进日志，并且不退出（消息循环下一轮照常跑）。
void report_uncaught(const char* where, const std::exception* e) {
    std::string msg = std::string("未捕获异常 @ ") + where;
    if (e != nullptr) {
        msg += "：";
        msg += e->what();
    }
    Logger::default_logger().error(msg);
}

/// 跟随系统的浅色/深色偏好。
///
/// freedesktop 这边没有 Win32 那样的注册表键，事实标准是
/// org.gnome.desktop.interface 的 color-scheme（GNOME 42+ / KDE 也实现了它）。
/// 拿不到就按浅色走 —— 这是刻意的：猜错成深色会让界面在浅色桌面上
/// 变成一块黑疙瘩，而猜错成浅色只是「没跟随」，观感损失小得多。
bool system_prefers_dark() {
    if (!process::command_exists("gsettings")) return false;
    auto r = process::run("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"},
                          {}, 2000);
    if (r.is_err() || r.value().exit_code != 0) return false;
    return r.value().output.find("dark") != std::string::npos;
}

namespace {

// -----------------------------------------------------------------------------
//  共享内存 buffer
// -----------------------------------------------------------------------------

/// 建一块匿名共享内存并映射出来。
///
/// 优先 memfd_create：它不落文件系统、没有残留、也不受 /dev/shm 大小限制。
/// 退回 shm_open 时要立刻 unlink，否则异常退出会在 /dev/shm 留一堆垃圾。
/// 这两个都是 Linux 专有 —— 这也是为什么缓冲区管理放在 shell 里而不是渲染器里。
int create_shm_file(size_t size) {
#ifdef MFD_CLOEXEC
    const int mfd = ::memfd_create("penhu-shm", MFD_CLOEXEC);
    if (mfd >= 0) {
        if (::ftruncate(mfd, static_cast<off_t>(size)) == 0) return mfd;
        ::close(mfd);
    }
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "/penhu-%d-%u", ::getpid(),
                  static_cast<unsigned>(size & 0xFFFFu));
    const int fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    ::shm_unlink(name);   // 立刻解除链接：fd 还在，文件已经在消失的路上
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// -----------------------------------------------------------------------------
//  键码映射：xkbcommon keysym → 我们的键码（数值与 Win32 VK 一致）
// -----------------------------------------------------------------------------

/// keysym → 键码。**只看 keysym，不看 keycode**：
/// keycode 是物理位置，随键盘布局变化（AZERTY 的 'a' 在别的物理键上），
/// 而 keysym 已经是「按当前布局解释出来的字符」—— 我们要的正是后者。
///
/// 返回 0 表示这个键我们不关心（字母数字之外的符号键目前都不用）。
unsigned keysym_to_key(uint32_t sym) {
    // 功能键：F1..F12 在 keysym 里是连续的
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
        return ui::key::F1 + (sym - XKB_KEY_F1);
    }
    // 可打印 ASCII：字母统一成**大写**（和 VK 码一致 —— VK_A 就是 'A'）。
    // 这样 Ctrl+C 判断只需要看 'C'，不用管 Shift 有没有按。
    if ((sym >= XKB_KEY_a && sym <= XKB_KEY_z) ||
        (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)) {
        return static_cast<unsigned>(sym & ~0x20u);
    }
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return static_cast<unsigned>(sym);
    if (sym == XKB_KEY_space) return ui::key::Space;

    switch (sym) {
        case XKB_KEY_BackSpace:  return ui::key::Backspace;
        case XKB_KEY_Tab:        return ui::key::Tab;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:   return ui::key::Enter;
        case XKB_KEY_Escape:     return ui::key::Escape;
        case XKB_KEY_Delete:     return ui::key::Delete;
        case XKB_KEY_Insert:     return ui::key::Insert;
        case XKB_KEY_Home:       return ui::key::Home;
        case XKB_KEY_End:        return ui::key::End;
        case XKB_KEY_Left:       return ui::key::Left;
        case XKB_KEY_Right:      return ui::key::Right;
        case XKB_KEY_Up:         return ui::key::Up;
        case XKB_KEY_Down:       return ui::key::Down;
        case XKB_KEY_Page_Up:    return ui::key::PageUp;
        case XKB_KEY_Page_Down:  return ui::key::PageDown;
        default:                 return 0;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  C 回调 → 成员函数
// -----------------------------------------------------------------------------

#define PENHU_WL_OBJ(ptr) static_cast<Shell*>(::wl_proxy_get_user_data( \
        reinterpret_cast<struct wl_proxy*>(ptr)))

struct WaylandBridge {
    // ---- wl_registry ----
    static void registry_global(void* data, wl_registry* reg, uint32_t name,
                                const char* iface, uint32_t version) {
        static_cast<Shell*>(data)->handle_registry_global(name, iface, version);
    }
    static void registry_global_remove(void*, wl_registry*, uint32_t) {
        // 我们不跟踪 hotplug：显示器/输入设备中途变化时，下次重连会重新绑定。
        // 对单窗口的桌面应用来说，为了这个去维护一套重新绑定逻辑不值得。
    }

    // ---- xdg_wm_base ----
    static void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
        // 必须回 pong，否则合成器会认为客户端失去响应并把它标成「未响应」。
        xdg_wm_base_pong(base, serial);
    }

    // ---- xdg_surface ----
    static void xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
        // 一定要 ack：不回的话合成器不会发下一帧的 configure，
        // 表现是「窗口尺寸永远停在上一次」。
        xdg_surface_ack_configure(surf, serial);
        static_cast<Shell*>(data)->handle_configure(0, 0, false, false);
    }

    // ---- xdg_toplevel ----
    static void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h,
                                   wl_array* states) {
        bool maximized = false;
        bool fullscreen = false;
        if (states != nullptr) {
            auto* p = static_cast<uint32_t*>(states->data);
            const size_t n = states->size / sizeof(uint32_t);
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == XDG_TOPLEVEL_STATE_MAXIMIZED) maximized = true;
                if (p[i] == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = true;
            }
        }
        static_cast<Shell*>(data)->handle_configure(w, h, maximized, fullscreen);
    }
    static void toplevel_close(void* data, xdg_toplevel*) {
        static_cast<Shell*>(data)->handle_close();
    }
    static void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
    static void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

    // ---- wl_seat ----
    static void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
        static_cast<Shell*>(data)->handle_seat_capabilities(seat, caps);
    }
    static void seat_name(void*, wl_seat*, const char*) {}

    // ---- wl_pointer ----
    static void pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x,
                              wl_fixed_t y) {
        // 悬停状态要清一下：指针重新进入时上一个 hovered_ 可能已经失效
    }
    static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        static_cast<Shell*>(data)->handle_pointer_leave();
    }
    static void pointer_motion(void* data, wl_pointer*, uint32_t,
                               wl_fixed_t x, wl_fixed_t y) {
        static_cast<Shell*>(data)->handle_pointer_motion(wl_fixed_to_double(x),
                                                         wl_fixed_to_double(y));
    }
    static void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t,
                               uint32_t button, uint32_t state) {
        static_cast<Shell*>(data)->handle_pointer_button(
            button, state == WL_POINTER_BUTTON_STATE_PRESSED, serial);
    }
    static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis,
                             wl_fixed_t value) {
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
        static_cast<Shell*>(data)->handle_pointer_axis(wl_fixed_to_double(value));
    }
    static void pointer_frame(void*, wl_pointer*) {}
    static void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
    static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

    // ---- wl_keyboard ----
    static void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd,
                                uint32_t size) {
        static_cast<Shell*>(data)->handle_keymap(format, fd, size);
    }
    static void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*,
                               wl_array*) {
        static_cast<Shell*>(data)->handle_keyboard_enter();
    }
    static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
        static_cast<Shell*>(data)->handle_keyboard_leave();
    }
    static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t,
                             uint32_t key, uint32_t state) {
        static_cast<Shell*>(data)->handle_key(key,
                                              state == WL_KEYBOARD_KEY_STATE_PRESSED);
    }
    static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                                   uint32_t latched, uint32_t locked, uint32_t group) {
        static_cast<Shell*>(data)->handle_modifiers(depressed, latched, locked, group);
    }
    static void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {
        // 不做客户端按键重复：合成器一般会自己发重复的 key 事件。
        // 自己再实现一遍会导致「按住删除键删得过快」或双倍重复。
    }

    // ---- wl_buffer ----
    static void buffer_release(void*, wl_buffer*) {
        // 合成器用完了。我们用的是双缓冲 + 每帧重建，
        // 这里只需要知道「那块内存可以复用了」—— 当前实现在 upload 前
        // 直接 destroy 旧 buffer，所以这个回调暂时不需要做事。
    }
};

// 监听器表。用函数局部 static 而不是全局：初始化顺序有保证。
namespace {
const wl_registry_listener kRegistryListener = {
    &WaylandBridge::registry_global, &WaylandBridge::registry_global_remove};
const xdg_wm_base_listener kWmBaseListener = {&WaylandBridge::wm_base_ping};
const xdg_surface_listener kXdgSurfaceListener = {&WaylandBridge::xdg_surface_configure};
const xdg_toplevel_listener kToplevelListener = {
    &WaylandBridge::toplevel_configure, &WaylandBridge::toplevel_close,
    &WaylandBridge::toplevel_configure_bounds, &WaylandBridge::toplevel_wm_capabilities};
const wl_seat_listener kSeatListener = {&WaylandBridge::seat_capabilities,
                                        &WaylandBridge::seat_name};
const wl_pointer_listener kPointerListener = {
    &WaylandBridge::pointer_enter,  &WaylandBridge::pointer_leave,
    &WaylandBridge::pointer_motion, &WaylandBridge::pointer_button,
    &WaylandBridge::pointer_axis,   &WaylandBridge::pointer_frame,
    &WaylandBridge::pointer_axis_source, &WaylandBridge::pointer_axis_stop,
    &WaylandBridge::pointer_axis_discrete};
const wl_keyboard_listener kKeyboardListener = {
    &WaylandBridge::keyboard_keymap, &WaylandBridge::keyboard_enter,
    &WaylandBridge::keyboard_leave,  &WaylandBridge::keyboard_key,
    &WaylandBridge::keyboard_modifiers, &WaylandBridge::keyboard_repeat_info};
const wl_buffer_listener kBufferListener = {&WaylandBridge::buffer_release};
}  // namespace

#undef PENHU_WL_OBJ

// -----------------------------------------------------------------------------
//  Shell
// -----------------------------------------------------------------------------

Shell& Shell::get() {
    static Shell instance;
    return instance;
}

bool Shell::init(const std::string& /*data_dir*/, std::wstring* err) {
    if (!renderer_.init()) {
        if (err != nullptr) *err = L"图形子系统初始化失败（Cairo / Pango / fontconfig）";
        return false;
    }
    if (!connect_wayland(err)) return false;
    if (!create_window(err)) return false;

    dark_ = system_prefers_dark();
    theme_ = dark_ ? ui::Theme::dark() : ui::Theme::light();
    App::get().dark = dark_;

    AppTree& tree = tree_;
    tree.request_minimize = []() {
        // Wayland 的 xdg-shell **没有**最小化请求（这是有意的：最小化是
        // 合成器/用户的事，客户端不该能把自己的窗口藏起来）。
        // 所以顶栏那个「最小化」按钮在 Linux 上不生效 —— 保留按钮是为了
        // 两平台界面一致，但要如实记一笔，免得以后有人当 bug 查。
        Logger::default_logger().info("最小化：Wayland 不支持客户端请求最小化，已忽略");
    };
    tree.request_toggle_maximize = []() {
        Shell& s = Shell::get();
        s.maximized_ = !s.maximized_;
        if (s.maximized_) {
            xdg_toplevel_set_maximized(s.toplevel_);
        } else {
            xdg_toplevel_unset_maximized(s.toplevel_);
        }
        s.rebuild_pages_safely();   // 图标要跟着变
        s.dirty_ = true;
    };
    tree.request_close = []() { Shell::get().should_close_ = true; };

    rebuild_pages_safely();
    return true;
}

bool Shell::connect_wayland(std::wstring* err) {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        if (err != nullptr) {
            *err = L"连不上 Wayland 合成器（WAYLAND_DISPLAY 没设置，或者不在图形会话里）";
        }
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    // 两轮 roundtrip：第一轮拿到全局对象清单，第二轮把它们的监听器都装上。
    // 只做一轮的话，seat 的 capabilities 事件会丢 —— 表现是「鼠标键盘完全没反应」。
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);

    if (compositor_ == nullptr || shm_ == nullptr) {
        if (err != nullptr) *err = L"合成器没提供 wl_compositor / wl_shm（不是标准 Wayland 环境？）";
        return false;
    }
    if (wm_base_ == nullptr) {
        // xdg-shell 是通行的窗口协议；没有它就开不了普通窗口。
        // 有些合成器只提供 wl_shell（早就废弃），那种情况我们不支持。
        if (err != nullptr) *err = L"合成器没提供 xdg_wm_base（xdg-shell 不可用）";
        return false;
    }

    xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    Logger::default_logger().info(
        "Wayland 已连接：compositor/shm/xdg-shell 就绪" +
        std::string(seat_ != nullptr ? "，输入设备可用" : "，但没拿到 seat（键鼠会没反应）"));
    return true;
}

void Shell::handle_registry_global(uint32_t name, const char* iface, uint32_t version) {
    const std::string s = iface != nullptr ? iface : "";
    if (s == wl_compositor_interface.name) {
        // 绑定时把支持的版本取小值：拿一个合成器不认识的版本号会直接断开连接
        compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
            registry_, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (s == wl_shm_interface.name) {
        shm_ = static_cast<wl_shm*>(
            wl_registry_bind(registry_, name, &wl_shm_interface, 1));
    } else if (s == xdg_wm_base_interface.name) {
        wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry_, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(wm_base_, &kWmBaseListener, this);
    } else if (s == wl_seat_interface.name) {
        seat_ = static_cast<wl_seat*>(
            wl_registry_bind(registry_, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(seat_, &kSeatListener, this);
    }
}

void Shell::handle_seat_capabilities(wl_seat* seat, uint32_t caps) {
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    const bool has_kbd = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    if (has_ptr && pointer_ == nullptr) {
        pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer_, &kPointerListener, this);
    } else if (!has_ptr && pointer_ != nullptr) {
        wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
    }

    if (has_kbd && keyboard_ == nullptr) {
        keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard_, &kKeyboardListener, this);
    } else if (!has_kbd && keyboard_ != nullptr) {
        wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
    }
}

bool Shell::create_window(std::wstring* err) {
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_ == nullptr) {
        if (err != nullptr) *err = L"创建 wl_surface 失败";
        return false;
    }

    // xdg_surface / xdg_toplevel 的两段式是 xdg-shell 的规矩：
    // xdg_surface 负责「configure/ack」这套事务，xdg_toplevel 负责窗口语义
    // （标题、最大化、调整大小）。分开是为了让未来的 popup 之类能复用前者。
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);

    xdg_toplevel_set_title(toplevel_, "PenHu Ledger · 每日花销");
    // app_id 是 Wayland 的「桌面文件名」：合成器用它匹配 .desktop、
    // 决定任务栏图标和窗口分组。名字必须和 packaging/penhu-ledger.desktop 一致，
    // 否则任务栏里会是一个没有图标的白块。
    xdg_toplevel_set_app_id(toplevel_, "penhu-ledger");

    // 首次 commit 才会让合成器发 configure。**必须等它**（在 run() 里等），
    // 不能急着 attach buffer：尺寸还不知道。
    wl_surface_commit(surface_);
    return true;
}

void Shell::handle_configure(int32_t w, int32_t h, bool maximized, bool fullscreen) {
    // configure 里的宽高是**建议值**，0 表示「你自己定」。
    // 我们自己定一个首选尺寸（逻辑 1240x860 换算成物理像素），
    // 但只应用一次：之后用户拖过窗口就尊重合成器给的值。
    if (w > 0 && h > 0) {
        pending_w_ = w;
        pending_h_ = h;
    } else if (!configured_) {
        // 首次且没给尺寸：按显示器缩放选一个合适的逻辑尺寸
        const int sw = 1240, sh = 860;
        pending_w_ = sw;
        pending_h_ = sh;
    }

    if (!configured_) {
        Logger::default_logger().info("收到首个 configure：目标尺寸 " +
                                      std::to_string(pending_w_) + "x" +
                                      std::to_string(pending_h_) + "（物理像素）");
    }
    if (maximized != maximized_ || fullscreen != fullscreen_) {
        maximized_ = maximized;
        fullscreen_ = fullscreen;
        sync_window_state();
    }
    configured_ = true;
}

void Shell::handle_close() { should_close_ = true; }

void Shell::sync_window_state() {
    // 顶栏的两个图标要反映当前状态，否则用户看不出自己在哪种模式里
    if (tree_.title == nullptr) return;
    tree_.title->maximized = maximized_;
    tree_.title->fullscreen = fullscreen_;
    dirty_ = true;
}

void Shell::handle_keymap(uint32_t format, int32_t fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ::close(fd);
        Logger::default_logger().warn("键盘布局格式不是 xkb_v1，键盘输入不可用");
        return;
    }
    // 键盘布局是通过一块共享内存传过来的：先 mmap 再解析成 xkb keymap。
    // 注意 map 的长度要含结尾的 NUL，但解析时只要 size 字节 ——
    // 两者差一个字节，用错在有些合成器上会读到越界。
    char* map = static_cast<char*>(::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (map == MAP_FAILED) return;

    if (xkb_ctx_ == nullptr) xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* km = xkb_keymap_new_from_string(xkb_ctx_, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
    ::munmap(map, size);
    if (km == nullptr) {
        Logger::default_logger().warn("键盘布局解析失败（xkb_keymap_new_from_string）");
        return;
    }
    if (xkb_keymap_ != nullptr) xkb_keymap_unref(xkb_keymap_);
    if (xkb_state_ != nullptr) xkb_state_unref(xkb_state_);
    xkb_keymap_ = km;
    xkb_state_ = xkb_state_new(km);
}

void Shell::handle_keyboard_enter() {}
void Shell::handle_keyboard_leave() {
    // 失去键盘焦点时把修饰键状态清掉。不清的话「按着 Shift 切窗口再回来」
    // 会让界面一直以为 Shift 还按着（表现为选不中/一直扩选）。
    ctrl_down_ = false;
    ui::set_modifier_state(false, false, false);
}

void Shell::handle_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked,
                             uint32_t group) {
    if (xkb_state_ == nullptr) return;
    xkb_state_update_mask(xkb_state_, depressed, latched, locked, 0, 0, group);
    const bool shift = xkb_state_mod_name_is_active(
                           xkb_state_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool ctrl = xkb_state_mod_name_is_active(
                          xkb_state_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool alt = xkb_state_mod_name_is_active(
                         xkb_state_, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    ctrl_down_ = ctrl;
    // 喂给 platform_input：Wayland 不允许客户端查询全局键盘状态，
    // 需要的控件（Shift+方向键扩选、Ctrl 组合键）必须先有这份状态。
    ui::set_modifier_state(shift, ctrl, alt);
}

void Shell::handle_key(uint32_t key_code, bool pressed) {
    if (!pressed || xkb_state_ == nullptr) return;

    // keycode + 8：Wayland 传的是 evdev 的原始码（须减 1 才是 xkb 的 code），
    // 而 xkb 的约定是 code = evdev + 8。所以净效果是 +8 —— 这个偏移
    // 写错的表现是「所有键都错位」，比如按回车出来的是别的字符。
    const xkb_keycode_t code = key_code + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, code);
    if (sym == XKB_KEY_NoSymbol) return;

    if (ctrl_pressed()) {
        if (dispatch_edit_shortcut(keysym_to_key(sym))) {
            dirty_ = true;
            return;
        }
    }

    const unsigned key_code_mapped = keysym_to_key(sym);
    if (key_code_mapped == 0) return;

    if (key_code_mapped == ui::key::Tab && ctrl_pressed()) {
        cycle_focus();
        dirty_ = true;
        return;
    }

    if (focus_ != nullptr && focus_->on_key(key_code_mapped, true)) {
        dirty_ = true;
        return;
    }

    // 没人消费：交给全局快捷键（F11 全屏）
    if (key_code_mapped == ui::key::F11) {
        fullscreen_ = !fullscreen_;
        if (fullscreen_) {
            xdg_toplevel_set_fullscreen(toplevel_, nullptr);
        } else {
            xdg_toplevel_unset_fullscreen(toplevel_);
        }
        sync_window_state();
        dirty_ = true;
    }
}

void Shell::handle_pointer_motion(double x, double y) {
    const ui::Point p = to_dip(x, y);
    last_pointer_ = p;   // 按钮/滚轮事件里要用（它们不一定带坐标）
    ui::Widget* hit = tree_.root.hit_test(p);
    if (hit != hovered_) {
        if (hovered_ != nullptr) hovered_->on_pointer_move(p, false);
        hovered_ = hit;
        if (hit != nullptr) hit->on_pointer_move(p, true);
        dirty_ = true;
    } else if (hit != nullptr) {
        hovered_->on_pointer_move(p, true);
        // 顶栏内部的 hover 变化也要重绘：鼠标从「最小化」滑到「关闭」时，
        // 命中的组件没变，但画面必须变（Windows 那边在 WM_MOUSEMOVE 里同样处理）
        dirty_ = true;
    }

    if (pressed_ != nullptr) {
        // 拖动中：所有移动都归按下的那个组件（滚动条的拖动依赖这一点）
        pressed_->on_pointer_move(p, true);
        dirty_ = true;
    }
}

void Shell::handle_pointer_leave() {
    if (hovered_ != nullptr) {
        hovered_->on_pointer_move({-1e6f, -1e6f}, false);
        hovered_ = nullptr;
        dirty_ = true;
    }
}

void Shell::handle_pointer_button(uint32_t button, bool pressed, uint32_t serial) {
    last_serial_ = serial;   // 拖动窗口要用它，且只能在这一刻用
    if (button != 0x110 /* BTN_LEFT */) return;   // 右中键暂时不用

    if (pressed) {
        const ui::Point p = last_pointer_;
        // 顶栏空白处 → 交给合成器拖窗口。
        //
        // 这是 Wayland 和 Win32 差别最大的地方：Win32 里返回 HTCAPTION 系统就
        // 接管拖动；Wayland 必须由客户端在**拿到输入 serial 时**主动请求
        // xdg_toplevel_move(seat, serial)，而且只能在按下事件处理期间发起 ——
        // 过了这个窗口 serial 就失效了。
        if (tree_.title != nullptr && tree_.title->bounds().contains(p) &&
            tree_.title->hit_for_drag(p)) {
            if (seat_ != nullptr) xdg_toplevel_move(toplevel_, seat_, last_serial_);
            return;
        }

        pressed_ = bubble_down(tree_.root.hit_test(p), p);
        set_focus(pressed_ != nullptr && pressed_->focusable ? pressed_ : nullptr);
        dirty_ = true;
    } else {
        const ui::Point p = last_pointer_;
        ui::Widget* hit = tree_.root.hit_test(p);
        if (pressed_ != nullptr) {
            // 按下与抬起必须在同一个组件上才算一次点击；否则给一个界外坐标，
            // 让组件自己取消（组件的 on_pointer_up 会检查坐标是否在自己范围内）。
            pressed_->on_pointer_up(hit == pressed_ ? p
                                                    : ui::Point{-100000.0f, -100000.0f});
        }
        pressed_ = nullptr;
        dirty_ = true;
    }
}

void Shell::handle_pointer_axis(double dy) {
    // Wayland 的 axis 值是「滚动量」（正数 = 向下），单位是一个「滚动步」的分数；
    // 乘 10 大致对上 Windows 那边一格 = WHEEL_DELTA(120)/120 = 1 的手感。
    const float delta = static_cast<float>(-dy * 10.0);
    if (bubble_wheel(tree_.root.hit_test(last_pointer_), last_pointer_, delta)) {
        dirty_ = true;
    }
}

ui::Point Shell::to_dip(double px, double py) const {
    // Wayland 的指针坐标是**表面本地坐标（物理像素）**，而绘制用的是 DIP，
    // 所以要除掉缩放。用 renderer_ 的 scale：它和创建 shm buffer 时用的是同一个值。
    const float s = renderer_.scale() > 0.0f ? renderer_.scale() : 1.0f;
    return {static_cast<float>(px) / s, static_cast<float>(py) / s};
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

bool Shell::dispatch_edit_shortcut(unsigned k) {
    if (focus_ == nullptr) return false;
    ui::EditCmd cmd;
    switch (k) {
        case 'C': cmd = ui::EditCmd::Copy;      break;
        case 'X': cmd = ui::EditCmd::Cut;       break;
        case 'V': cmd = ui::EditCmd::Paste;     break;
        case 'A': cmd = ui::EditCmd::SelectAll; break;
        default:  return false;
    }
    return focus_->on_edit_command(cmd);
}

void Shell::rebuild_pages_safely() {
    hovered_ = nullptr;
    pressed_ = nullptr;
    set_focus(nullptr);
    tree_.rebuild_pages(App::get());
}

void Shell::request_repaint() {
    dirty_ = true;
    // 从别的线程调时唤醒事件循环：wl_display 本身不是线程安全的，
    // 但 wl_display_prepare_read / cancel_read 这一对可以在别的线程里用来
    // 打断阻塞的 wl_display_dispatch。这里只是置脏标记 + 让 dispatch 提前返回。
    if (display_ != nullptr) {
        char buf[1] = {0};
        // 用一个空写把 poll 唤醒；EAGAIN 表示队列已满，那本来就会立刻返回。
        (void)!::write(wl_display_get_fd(display_), buf, 0);
    }
}

void Shell::relayout() {
    if (pending_w_ <= 0 || pending_h_ <= 0) return;

    // 缩放因子：用 renderer_ 当前的值（首帧由 init 时定）。
    // 高 DPI 显示器上合成器给的 buffer scale 会是 2 —— 那属于 HiDPI 支持，
    // 这里先按 1.0 走（WSLg 也是 1.0），有需要再补 wp_fractional_scale。
    const float s = renderer_.scale() > 0.0f ? renderer_.scale() : 1.0f;
    renderer_.resize(pending_w_, pending_h_);

    // 窗口宽度跨过档位时重建页面：多栏/单栏是**构建期**决定的，
    // 不重建的话拖窗口大小不会改变布局。
    App& app = App::get();
    const float w = renderer_.logical_size().w;
    const WidthClass want = (w < 820.0f) ? WidthClass::Compact
                                         : (w < 1180.0f ? WidthClass::Medium
                                                        : WidthClass::Expanded);
    if (want != app.width_class) {
        app.width_class = want;
        rebuild_pages_safely();
    }
    (void)s;

    if (!ensure_shm_buffer(pending_w_, pending_h_)) {
        Logger::default_logger().error("wl_shm 缓冲分配失败，界面无法显示");
    }
}

bool Shell::ensure_shm_buffer(int w_px, int h_px) {
    if (buffer_ != nullptr && w_px == buffer_w_ && h_px == buffer_h_) return true;

    release_shm_buffer();

    const size_t stride = static_cast<size_t>(w_px) * 4;
    const size_t size = stride * static_cast<size_t>(h_px);
    const int fd = create_shm_file(size);
    if (fd < 0) {
        Logger::default_logger().error("memfd/shm_open 失败：" + std::string(std::strerror(errno)));
        return false;
    }

    void* data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int32_t>(size));
    // 格式必须是 ARGB8888：渲染器输出的是 cairo 的 ARGB32，
    // 在小端机器上就是 BGRA 预乘，和这个格式的内存布局一致（和 Windows 侧的
    // GUID_WICPixelFormat32bppPBGRA 也对得上 —— 三处一致才不用做格式转换）。
    buffer_ = wl_shm_pool_create_buffer(pool, 0, w_px, h_px,
                                        static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);   // 池子已经持有了对内存的引用，fd 可以关了

    if (buffer_ == nullptr) {
        ::munmap(data, size);
        return false;
    }
    wl_buffer_add_listener(buffer_, &kBufferListener, this);

    buffer_data_ = static_cast<uint8_t*>(data);
    buffer_size_ = size;
    buffer_w_ = w_px;
    buffer_h_ = h_px;
    return true;
}

void Shell::release_shm_buffer() {
    if (buffer_ != nullptr) {
        wl_buffer_destroy(buffer_);
        buffer_ = nullptr;
    }
    if (buffer_data_ != nullptr) {
        ::munmap(buffer_data_, buffer_size_);
        buffer_data_ = nullptr;
    }
    buffer_size_ = 0;
    buffer_w_ = buffer_h_ = 0;
}

void Shell::upload_and_commit() {
    if (buffer_ == nullptr || buffer_data_ == nullptr) return;

    int stride_px = 0;
    const uint8_t* src = renderer_.pixels(&stride_px);
    if (src == nullptr) return;

    const int w = std::min(buffer_w_, renderer_.pixel_width());
    const int h = std::min(buffer_h_, renderer_.pixel_height());

    // 逐行拷贝而不是一次 memcpy：cairo 的行距和 buffer 的行距可能不同
    // （cairo 会把 stride 对齐到 4 的倍数，我们这边 stride = w*4，正常的宽度下一致，
    //  但不能假定 —— 差一个字节就会让整幅图斜着错位，而且很难看出原因）。
    const size_t dst_stride = static_cast<size_t>(buffer_w_) * 4;
    const size_t src_stride = static_cast<size_t>(stride_px) * 4;
    if (dst_stride == src_stride && static_cast<int>(h) == buffer_h_) {
        std::memcpy(buffer_data_, src, static_cast<size_t>(w) * 4 * static_cast<size_t>(h));
    } else {
        for (int y = 0; y < h; ++y) {
            std::memcpy(buffer_data_ + static_cast<size_t>(y) * dst_stride,
                        src + static_cast<size_t>(y) * src_stride,
                        static_cast<size_t>(w) * 4);
        }
    }

    wl_surface_attach(surface_, buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, w, h);
    wl_surface_commit(surface_);
    wl_display_flush(display_);

    {
        static int committed = 0;
        if (++committed == 1) {
            Logger::default_logger().info(
                "首帧已提交给合成器：" + std::to_string(w) + "x" + std::to_string(h) + " 像素");
        }
    }
}

void Shell::paint() {
    App& app = App::get();
    tree_.sync(app, monotonic_ms());

    const ui::Size size = renderer_.logical_size();
    if (!renderer_.begin_frame(theme_.palette.surface)) return;

    tree_.root.layout({0.0f, 0.0f, size.w, size.h}, renderer_);
    tree_.root.paint(renderer_, theme_);

    if (!renderer_.end_frame()) return;
    // Linux 上 present() 是空操作，像素要我们自己交给合成器
    upload_and_commit();
    dirty_ = false;
}

int Shell::run() {
    // 首帧：等合成器把尺寸告诉我们（create_window 里 commit 的那一次）
    if (wl_display_roundtrip(display_) < 0) {
        Logger::default_logger().error("与合成器的首次握手失败");
        return 1;
    }

    // 缩放因子：Wayland 的 buffer scale 概念在这里简化为 1.0（见 relayout 的说明）。
    const float scale = 1.0f;
    renderer_.attach_window(surface_, scale);
    relayout();

    if (!configured_) {
        // 有些合成器第一次 configure 不给尺寸，那就用首选尺寸自己定一个
        pending_w_ = 1240;
        pending_h_ = 860;
        relayout();
    }

    while (!should_close_) {
        // 有脏标记就画一帧。Wayland 没有「失效区域」这回事，
        // 所以重绘完全由自己的 dirty_ 驱动。
        if (dirty_ && renderer_.has_target()) paint();

        // 先处理已经到达的事件（非阻塞），再决定要不要 sleep
        if (wl_display_prepare_read(display_) != 0) {
            // 队列里还有事件没读完：dispatch 一次把它们处理掉，不要睡
            wl_display_dispatch_pending(display_);
            continue;
        }
        wl_display_flush(display_);

        // poll 一段时间就醒：光标闪烁、提示条过期、动画都靠这个 60Hz 左右的
        // 节拍推进。纯靠事件驱动的话，闪烁就不会动（没有事件时进程在 sleep）。
        //
        // 用 poll 而不是 wl_display_dispatch：后者会一直阻塞到有事件，
        // 那样光标记就不会闪、提示条也不会自己消失。
        struct pollfd pfd{};
        pfd.fd = wl_display_get_fd(display_);
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 16);   // 16ms ≈ 60fps

        if (pr > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(display_);
            wl_display_dispatch_pending(display_);
        } else {
            wl_display_cancel_read(display_);
        }

        // 每帧推进一次时间相关的状态（光标闪烁、提示条过期、动画插值）
        const uint64_t now = monotonic_ms();
        const float dt = static_cast<float>(now - last_tick_ms_);
        last_tick_ms_ = now;
        if (dt > 0.0f) {
            if (focus_ != nullptr) {
                if (auto* tf = dynamic_cast<ui::TextField*>(focus_)) tf->tick(dt);
            }
            if (tree_.root.tick(dt)) dirty_ = true;
        }
        // 提示条：到点了要重绘才能让它消失
        if (App::get().toast_alive(now)) {
            // 提示条还在显示 → 保持节拍（光标记样式的内容会变）
        } else if (!App::get().toast.empty()) {
            dirty_ = true;
        }
        if (now - last_caret_ms_ >= 500) {
            last_caret_ms_ = now;
            if (focus_ != nullptr) {
                if (auto* tf = dynamic_cast<ui::TextField*>(focus_)) tf->tick_caret();
                dirty_ = true;
            }
        }
    }

    release_shm_buffer();
    if (keyboard_ != nullptr) wl_keyboard_destroy(keyboard_);
    if (pointer_ != nullptr) wl_pointer_destroy(pointer_);
    if (seat_ != nullptr) wl_seat_destroy(seat_);
    if (toplevel_ != nullptr) xdg_toplevel_destroy(toplevel_);
    if (xdg_surface_ != nullptr) xdg_surface_destroy(xdg_surface_);
    if (surface_ != nullptr) wl_surface_destroy(surface_);
    if (wm_base_ != nullptr) xdg_wm_base_destroy(wm_base_);
    if (shm_ != nullptr) wl_shm_destroy(shm_);
    if (compositor_ != nullptr) wl_compositor_destroy(compositor_);
    if (registry_ != nullptr) wl_registry_destroy(registry_);
    if (xkb_state_ != nullptr) xkb_state_unref(xkb_state_);
    if (xkb_keymap_ != nullptr) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_ != nullptr) xkb_context_unref(xkb_ctx_);
    wl_display_disconnect(display_);
    return 0;
}

void Shell::configure_diag(const std::string& user, const std::string& pass,
                           const std::string& shot_path, const std::string& scan_dir) {
    diag_user_ = user;
    diag_pass_ = pass;
    diag_shot_ = shot_path;
    diag_scan_dir_ = scan_dir;
    g_diag_mode = true;
}

// -----------------------------------------------------------------------------
//  离屏截图模式
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
    // 显式给个展开态：截图模式不跑事件循环，不会去读账号里存的折叠偏好 ——
    // 那样截图内容就取决于「这个账号上次点了什么」，同一份代码截出来的图会飘。
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
        // 而截图模式没有事件循环 —— 不推的话截到的是过渡中间态，
        // 「折叠后应该多宽」这种结论就完全对不上了。
        tree.root.tick(500.0f);
        if (!rr.begin_frame(th.palette.surface)) return false;
        tree.root.layout({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)}, rr);
        tree.root.paint(rr, th);
        if (!rr.end_frame()) return false;
        const std::string path = out_dir + "/" + name + ".png";
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
    // 设置页很长，再截一张下半部分 —— 只看首屏的话，新加的设置项根本不出现在验证图里
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
    // （比如信息行的次要文字色），只把 shoot 的绘制参数换成深色是不够的。
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
