#pragma once
// =============================================================================
//  native/app/shell.hpp
//  窗口、消息循环、输入分发、渲染循环。
//
//  两个模式共用同一棵树（AppTree）：
//    · 窗口模式  —— 真实 HWND + 鼠标键盘
//    · 截图模式  —— 离屏 WIC 位图 + 无人值守
//  共用是刻意的：如果截图走一套单独的绘制代码，"截图好看"就不能证明
//  "窗口里好看"。现在两者只差一个渲染目标。
//
//  输入与重绘的分工（这里容易搞错，所以写清楚）：
//    · 鼠标 / 键盘 / 输入法事件 → 只 InvalidateRect 重绘，**不重建组件树**
//      （重建会把正在输入的 TextField 换掉，光标和未提交的输入都会丢）
//    · App 主动请求的重绘（切页 / 数据变化 / 提示条）→ 重建树 + 重绘
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
// 只用到指针，所以前置声明即可 —— 不必把 wayland / xkbcommon 的头
// 拖进每一个包含 shell.hpp 的翻译单元。
struct wl_display; struct wl_registry; struct wl_compositor; struct wl_shm;
struct wl_seat; struct wl_pointer; struct wl_keyboard; struct wl_surface;
struct wl_buffer;
struct xdg_wm_base; struct xdg_surface; struct xdg_toplevel;
struct xkb_context; struct xkb_keymap; struct xkb_state;
#endif

#include "app/app.hpp"
#include "ui/controls.hpp"
#include "ui/foundation.hpp"
#include "ui/renderer.hpp"
#include "ui/titlebar.hpp"

namespace penhu::native {

/// 整棵界面树。窗口模式与截图模式都用它。
///
/// 子节点用裸指针持有：它们的实际所有权在 root 的 children_ 里
/// （Widget::add 收 unique_ptr），这里保存指针只是为了布局时能直接访问，
/// 不再多一层 unique_ptr 和随之而来的空指针检查。
/// 诊断模式开关（由 configure_diag 置位）。
/// 作用是**抑制错误弹框** —— 无人值守跑自动化时，一个等待点击的模态框
/// 会把整个流程挂死，比不弹更糟。日志照样写。
extern bool g_diag_mode;

struct AppTree {
    /// 根是纵向的：最上面一条应用自定义顶栏，下面是「左侧导航 + 右侧内容」。
    ///
    /// 顶栏横跨整个窗口宽度（而不是只盖内容区）：侧边栏上方留一块空白会很怪，
    /// 而且窗口的拖动热区本来就该是整条顶边。
    /// （更早的版本是「内容 + 底部 tab」的手机形态，那时根是纵向的；
    /// 改成侧边栏后根一度是横向，加了顶栏又回到纵向。）
    ui::VBox      root;
    ui::TitleBar* title{nullptr};
    ui::NavRail*  rail{nullptr};
    ui::Stack*    overlay{nullptr};     // 页面 + 浮层
    ui::VBox*     page_host{nullptr};   // 当前页面填这里
    ui::Snackbar* snack{nullptr};

    /// 顶栏按钮 → 窗口操作。由 Shell 注入。
    /// 走回调而不是直接调用 Shell：截图模式用的是局部的 AppTree，
    /// 那时没有任何窗口可操作，回调留空即可（点了也不会崩）。
    std::function<void()> request_minimize;
    std::function<void()> request_toggle_maximize;
    std::function<void()> request_close;

    /// 搭骨架，只做一次
    void assemble(App& app);
    /// 按 app.screen 重建页面部分
    void rebuild_pages(App& app);
    /// 同步侧边栏选中态、折叠状态、顶栏文字与提示条可见性（每帧调用，便宜）
    void sync(App& app, uint64_t now_ms);
};

// -----------------------------------------------------------------------------
//  Shell —— 平台相关的那一半
//
//  两个平台各有一个 Shell，公开接口一致（init / run / request_repaint /
//  configure_diag），实现完全不同：
//    Windows：Win32 窗口 + 消息循环 + WM_* 分发 + 无边框窗口的命中测试
//    Linux：  Wayland（xdg-shell）+ wl_shm 缓冲 + xkbcommon 键盘
//  共用的那一半（AppTree、事件冒泡、诊断辅助）在 shell_common.cpp。
//
//  分成两个类而不是一个 #ifdef 一大堆成员：两边的窗口概念差别太大
//  （HWND + WM_* vs wl_surface + 事件回调），硬塞进一个类只会让两边都难读。
// -----------------------------------------------------------------------------
#ifdef _WIN32
class Shell {
public:
    static Shell& get();

    bool init(HINSTANCE hinst, const std::string& data_dir, std::wstring* err);
    int  run();

    /// App 请求重绘时调它（内部 PostMessage，线程安全）
    void request_repaint();
    HWND hwnd() const { return hwnd_; }

    /// 诊断模式：开窗后自动登录指定账号，把真实窗口的某一帧存成 PNG 再退出。
    /// 存在的理由很实际：**登录之后进记账页的那段渲染是崩过的地方**，
    /// 而正常模式需要人手输入密码 —— 没有这个模式就只能靠人反复试。
    void configure_diag(const std::string& user, const std::string& pass,
                        const std::string& shot_path, const std::string& scan_dir);

private:
    Shell() = default;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void   paint();
    void   relayout();
    /// 重建页面。**必须走这个函数，不要直接调 tree_.rebuild_pages** ——
    /// 它会先清掉 hovered_ / pressed_ / focus_ 这三个可能指向旧树的指针。
    ///
    /// 不清的后果是真实的崩溃：重建会销毁整棵页面子树，而那三个指针还指着
    /// 里面的组件；下一次鼠标移动或按键就写到已释放的内存里。
    /// 「点一下就闪退」正是这么来的。顺序很重要：先清指针，再重建。
    void   rebuild_pages_safely();
    /// 把窗口的当前状态同步到顶栏图标上（普通 / 最大化 / 全屏三种）。
    /// 状态一变就要调，否则用户看不出自己现在在哪一种里。
    void   sync_window_state();
    ui::Point to_dip(int px, int py) const;
    void   set_focus(ui::Widget* w);
    void   cycle_focus();
    void   handle_ime(HWND hwnd, LPARAM lp);
    float  dpi_scale() const;

    /// 无边框窗口的 DWM 设置：Win11 原生圆角、边框色跟随主题、关掉系统过渡动画。
    /// 主题切换后要重新调用 —— 边框色是跟着主题走的。
    void   apply_window_frame();
    bool   is_maximized() const;
    void   toggle_maximize();
    /// 全屏是**独立于最大化**的一种状态：最大化占满工作区（不压任务栏），
    /// 全屏占满整个显示器（盖住任务栏）。两者不能混为一谈。
    bool   is_fullscreen() const { return fullscreen_; }
    void   toggle_fullscreen();

    /// 开始一段窗口矩形动画（最大化 / 还原 / 全屏都走它）。
    ///
    /// 做法是先算好目标矩形，再把窗口从**当前实际位置**逐帧推过去；
    /// 动画期间窗口并不处于目标状态，只有最后一帧才真正 ShowWindow 切状态。
    /// 这条很关键：提前切状态会让系统把 rcNormalPosition 记错，
    /// 表现为「从最大化还原后位置和尺寸漂了」。
    void   start_window_anim(const RECT& to, bool end_maximized, bool end_fullscreen);
    /// 每帧推进动画。返回是否还在动。
    bool   tick_window_anim(uint64_t now_ms);
    /// 最大化时的目标**窗口**矩形（不是客户区，见 .cpp 的说明）
    RECT   rect_for_maximize() const;
    /// 全屏时的目标矩形 = 整个显示器（含任务栏区域）
    RECT   rect_for_fullscreen() const;

    /// 把 Ctrl 组合键翻译成编辑命令，交给当前焦点组件。true = 已被消费。
    bool   dispatch_edit_shortcut(unsigned vk);
    /// Ctrl 是否按下。自己维护的状态 + 系统真实状态，两者取或。
    bool   ctrl_pressed() const;

    HWND hwnd_{nullptr};
    ui::Renderer     renderer_;
    ui::Theme        theme_;
    AppTree          tree_;
    ui::Widget*      hovered_{nullptr};
    ui::Widget*      pressed_{nullptr};
    ui::Widget*      focus_{nullptr};
    uint64_t         last_tick_ms_{0};     // 上一帧时间，用来算真实 dt
    uint64_t         last_caret_ms_{0};    // 上次光标闪烁翻转的时间
    /// 上一次重建时所在的页面。用来判断「这次重建是同页刷新还是切页」——
    /// 同页刷新要保住滚动位置，切页则应该从头看。
    Screen           last_screen_{Screen::Login};

    /// 窗口矩形动画（最大化 / 还原 / 全屏）
    struct WindowAnim {
        bool     active{false};
        RECT     from{};
        RECT     to{};
        uint64_t start_ms{0};
        bool     end_maximized{false};
        bool     end_fullscreen{false};
        int      frames{0};        // 实际画了几帧（用来发现掉帧）
        /// 动画开始时的渲染目标构建计数。结束时一减 = 动画途中重建了几次
        /// （start_window_anim 已按全程最大尺寸 reserve，正常必须是 0）。
        int      builds0{0};
        /// 动画开始时的窗口摆放信息。
        /// 手动最大化后收尾不再切系统状态，这里只留作诊断取值用。
        WINDOWPLACEMENT saved{};
    };
    WindowAnim       win_anim_;
    bool             fullscreen_{false};
    // 手动最大化：窗口矩形直接动画到工作区，**不**进系统的 zoomed 态。
    // 原因：系统 SW_MAXIMIZE 会把无边框窗口的矩形外扩到显示器之外，
    // Windows 检测到「无边框窗口覆盖整个显示器」会判定为全屏应用、
    // 自动隐藏任务栏 —— 露出来的就是我们没画的外扩环（用户看到的黑条）。
    // 自己把窗口摆到 rcWork：矩形 < 显示器，任务栏保留，客户区连续无跳变。
    bool  manual_max_{false};
    RECT  saved_rect_{};              // 进最大化前的窗口矩形（还原目标）
    /// 进入全屏**之前**的窗口矩形。
    /// 不能等退出时再问系统要：全屏期间窗口处在 normal 态且矩形被改大了，
    /// 系统会把 rcNormalPosition 更新成全屏尺寸，于是「退出全屏」会退到
    /// 一个和全屏一样大的矩形 —— 表现为按 F11 毫无反应。
    RECT             rect_before_fullscreen_{};
    /// 进全屏前是不是最大化态（退出时要还原成最大化，而不是普通窗口）
    bool             was_max_before_fullscreen_{false};
    /// 动画期间被推迟的页面重建。跨栏档位时（单栏↔多栏）才会发生 ——
    /// 每帧重建整棵页面树太重，记下来等动画结束补一次。
    bool             pending_rebuild_{false};
    /// 用户正在拖拽窗口（缩放或移动）。系统在这期间进入模态循环，
    /// WM_SIZE 会密集到来 —— 用它把重建页面这类重活推迟到松手之后。
    bool             resizing_{false};
    bool             dark_{false};

    /// Ctrl 键是否按下（自己维护的那一份）。
    ///
    /// 为什么不直接用 GetKeyState：它读的是**硬件**键盘状态，而诊断模式
    /// 靠注入窗口消息模拟按键 —— 注入的 Ctrl 在 GetKeyState 里看不见。
    /// 自己记一份，注入的消息也能被正确识别；再用 GetKeyState 兜底取或，
    /// 免得漏掉 KEYUP 时状态永久卡住（那会表现为「随便按个 V 就开始粘贴」）。
    bool ctrl_down_{false};

    /// 诊断自动化的一帧（WM_TIMER 逐帧驱动）。实现在 shell_diag.cpp：
    /// 1100 多行状态机只在 --diag-* 参数下运行，与正常路径零共用，
    /// 留在 handle() 里会把消息分发淹掉。
    void tick_diag(HWND hwnd, float dt_ms);

    // 诊断模式状态（-1 = 没开）
    std::string diag_user_;
    std::string diag_pass_;
    std::string diag_shot_;
    std::string  diag_before_id_;   // 注入点击前的选中分类，用来判断点击有没有生效
    std::wstring diag_paste_before_; // 注入 Ctrl+V 前的 set_api_key，用来判断粘贴有没有生效
    float        diag_offset_before_{0.0f};  // 注入滚动前的 offset，用来判断滚动有没有生效
    bool         diag_rail_ok_{false};       // 侧边栏两项断言是否通过
    bool         diag_frame_ok_{false};      // 无边框窗口之前那批断言是否通过
    bool         diag_title_drag_ok_{false}; // 顶栏空白处是否返回 HTCAPTION
    bool         diag_title_btn_ok_{false};  // 控制按钮上是否返回 HTCLIENT
    bool         diag_no_frame_ok_{false};   // 非客户区是否为 0（系统标题栏真的没了）
    RECT          diag_restore_rect_{};    // 最大化之前系统记录的「正常矩形」
    bool          diag_restore_ok_{false}; // 还原后是否回到原位置原尺寸
    RECT          diag_mon_{};          // 当前显示器的完整矩形（不含任务栏扣除）
    RECT          mi_work_{};           // 当前显示器的工作区
    bool          diag_full_ok_{false}; // 全屏后客户区是否等于整个显示器
    bool          diag_exit_full_ok_{false};  // 退出全屏后是否回到原矩形
    int          diag_seq_n_{0};        // 已收集的矩形帧数
    std::string  diag_max_seq_;         // 最大化过程中逐帧的窗口矩形
    bool         diag_max_ok_{false};        // 最大化后客户区是否正好填满工作区
    bool         diag_resize_ok_{false};     // 连续缩放后尺寸与渲染目标是否一致
    int          diag_resize_ms_{0};         // 连续缩放的总耗时（用来算每一步的重绘成本）
    /// paint() 被调用的次数。诊断的「连续缩放」用它量一件事：
    /// 每一步尺寸变化**当场就画完了没有**。差值为 0 意味着那一步窗口是
    /// 「已经变大但还没画」的状态 —— 露出来的就是背景色，即肉眼看到的白闪。
    int          paint_count_{0};
    bool         diag_chain_ok_{true};       // 最大化→全屏→最大化→还原 这条链是否全对
    bool         diag_snack_ok_{false};      // 提示条穿透是否通过（与滚动断言合并判定）
    size_t       diag_records_before_{0};    // 入账前的记录数（多图入库断言用）
    int          diag_scan_first_{0};        // 目录首扫新增数（去重断言用）
    std::wstring diag_scan_dir_;             // 诊断用的扫描目录（不传就用内置测试目录）
    bool         scan_pre_clean_exists_{false};  // 清理验证：入账前文件是否真的存在
    bool         diag_rail_before_{false};       // 侧边栏折叠前的展开状态
    int          diag_screen_before_{-1};        // 点导航项之前所在的页面
    int          diag_stage_{-1};
    /// 诊断用的时间累加器（毫秒）。**不要改回「第 N 次 tick」**：
    /// 帧率一变，按计数写的等待时长就跟着变，诊断会莫名其妙地过早或过晚。
    float        diag_ms_{0.0f};
};

#else
// -----------------------------------------------------------------------------
//  Linux 侧：Wayland
//
//  和 Windows 那份的差别不只是 API，有三件事的**做法本身就不同**：
//
//  1) **窗口尺寸不是自己能定的**。Windows 用 SetWindowPos 想多大多大；
//     Wayland 里尺寸由合成器通过 xdg_toplevel.configure 告诉客户端，
//     客户端只能在自己想要的尺寸和合成器给的尺寸之间「协商」。
//     所以 init() 里没有「按工作区算一个尺寸」这段 —— 那是 Wayland 不允许的。
//
//  2) **绘制结果要主动上传**。Windows 是往窗口 DC 贴位图；Wayland 必须把
//     像素写进一块 wl_shm 共享内存 buffer、attach 给 surface、再 commit。
//     所以 Shell 持有 wl_buffer，renderer_.present() 在这边是空操作。
//
//  3) **每帧要等合成器通知**。wayland 用 frame callback 做节流：
//     画完一帧要等合成器回一个 done，才能画下一帧。不等的话会一直重绘，
//     白白吃掉 CPU（Windows 那边是靠 WM_TIMER + 按需失效，机制完全不同）。
//
//  共同点：输入分发的语义完全一致（冒泡、焦点、Ctrl 组合键），
//  因为那部分代码在 shell_common.cpp 与 ui/ 里，两边共用。
// -----------------------------------------------------------------------------
class Shell {
public:
    static Shell& get();

    bool init(const std::string& data_dir, std::wstring* err);
    int  run();

    /// App 请求重绘时调它（线程安全：只置脏标记并唤醒事件循环）
    void request_repaint();

    /// 诊断模式：开窗后自动登录指定账号，把真实窗口的某一帧存成 PNG 再退出。
    void configure_diag(const std::string& user, const std::string& pass,
                        const std::string& shot_path, const std::string& scan_dir);

private:
    Shell() = default;

    /// Wayland 的事件是 C 回调（wl_*_listener 里是一串函数指针），
    /// 需要直接驱动下面的处理函数。与其把七八个回调都写成 static 成员
    /// （那要在头文件里写一长串函数指针类型，噪音很大），
    /// 不如用一个内部结构体做友元，把「C 回调 → 成员函数」这层翻译
    /// 全部关在 shell_linux.cpp 里。
    friend struct WaylandBridge;

    bool connect_wayland(std::wstring* err);
    bool create_window(std::wstring* err);
    /// 按需要（重新）分配 wl_shm 缓冲。尺寸变了必须重建 ——
    /// wl_buffer 的尺寸在创建时就固定了，不能改。
    bool ensure_shm_buffer(int w_px, int h_px);
    void release_shm_buffer();
    /// 把渲染器画好的像素拷进 wl_buffer 并提交给合成器
    void upload_and_commit();

    void paint();
    void relayout();
    /// 重建页面。**必须走这个函数，不要直接调 tree_.rebuild_pages** ——
    /// 它会先清掉 hovered_ / pressed_ / focus_ 这三个可能指向旧树的指针，
    /// 否则下一次鼠标移动就写到已释放的内存里（Windows 那边「点一下就闪退」
    /// 就是这么来的，Linux 同理）。
    void rebuild_pages_safely();
    void set_focus(ui::Widget* w);
    void cycle_focus();
    ui::Point to_dip(double px, double py) const;

    void handle_registry_global(uint32_t name, const char* interface, uint32_t version);
    void handle_seat_capabilities(wl_seat* seat, uint32_t caps);
    void handle_configure(int32_t w, int32_t h, bool maximized, bool fullscreen);
    void handle_close();
    void handle_pointer_motion(double x, double y);
    void handle_pointer_leave();
    void handle_pointer_button(uint32_t button, bool pressed, uint32_t serial);
    void handle_pointer_axis(double dy);
    void handle_keymap(uint32_t format, int32_t fd, uint32_t size);
    void handle_keyboard_enter();
    void handle_keyboard_leave();
    void handle_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked,
                          uint32_t group);
    /// key_code 是 evdev 原始码（Wayland 给的），不是 keysym
    void handle_key(uint32_t key_code, bool pressed);
    /// 把当前的最大化/全屏状态同步到顶栏图标上
    void sync_window_state();
    bool dispatch_edit_shortcut(unsigned vk);
    bool ctrl_pressed() const { return ctrl_down_; }

    /// Wayland 对象。用前置声明而不是 include wayland 的头：
    /// shell.hpp 会被 main 之类的翻译单元包含，没必要让它们也依赖协议头。
    wl_display*    display_{nullptr};
    wl_registry*   registry_{nullptr};
    wl_compositor* compositor_{nullptr};
    wl_shm*        shm_{nullptr};
    wl_seat*       seat_{nullptr};
    wl_pointer*    pointer_{nullptr};
    wl_keyboard*   keyboard_{nullptr};
    wl_surface*    surface_{nullptr};
    /// xdg-shell 的三件套：wm_base 是协议入口（负责 ping/pong），
    /// xdg_surface 管 configure 事务，xdg_toplevel 才是「窗口」语义
    /// （标题、最大化、全屏、移动/缩放请求）。
    xdg_wm_base*   wm_base_{nullptr};
    xdg_surface*   xdg_surface_{nullptr};
    xdg_toplevel*  toplevel_{nullptr};

    /// shm 缓冲。**双缓冲**：一块正被合成器读，另一块给我们写。
    /// 单缓冲会出现「合成器正在读、我们同时在写」——屏幕上就是撕裂。
    /// （Windows 那边靠 WS_EX_COMPOSITED 达到同样目的，问题相同、机制不同。）
    wl_buffer*     buffer_{nullptr};
    uint8_t*       buffer_data_{nullptr};
    size_t         buffer_size_{0};
    int            buffer_w_{0};
    int            buffer_h_{0};

    /// 键盘：xkbcommon 负责「keycode + 布局 → keysym」，这样才有中文输入法以外
    /// 的所有键（比如 Ctrl+C）可用。布局由合成器通过 wl_keyboard.keymap 下发。
    xkb_context*   xkb_ctx_{nullptr};
    xkb_keymap*    xkb_keymap_{nullptr};
    xkb_state*     xkb_state_{nullptr};

    ui::Renderer     renderer_;
    ui::Theme        theme_;
    AppTree          tree_;
    ui::Widget*      hovered_{nullptr};
    ui::Widget*      pressed_{nullptr};
    ui::Widget*      focus_{nullptr};
    uint64_t         last_tick_ms_{0};     // 上一帧时间，用来算真实 dt
    uint64_t         last_caret_ms_{0};    // 上次光标闪烁翻转的时间
    /// 上一次重建时所在的页面。用来判断「这次重建是同页刷新还是切页」——
    /// 同页刷新要保住滚动位置，切页则应该从头看。
    Screen           last_screen_{Screen::Login};

    /// 合成器给的待用尺寸（物理像素）。不等于当前 buffer 尺寸，
    /// 要在 configure 之后的下一帧才真正生效。
    int    pending_w_{0};
    int    pending_h_{0};
    bool   configured_{false};
    /// 需要重绘。Wayland 没有 InvalidateRect，自己维护一个脏标记。
    bool   dirty_{true};
    bool   should_close_{false};
    bool   maximized_{false};
    bool   fullscreen_{false};
    /// Ctrl 是否按下。Wayland 拿不到全局键盘状态，只能自己记。
    bool   ctrl_down_{false};
    bool   dark_{false};

    /// 最近一次指针位置（DIP）与最近一次输入事件的 serial。
    ///
    /// serial 必须留着：xdg_toplevel_move / resize 只能在**收到输入事件的那一刻**
    /// 用当时的 serial 发起，过了那个时机 serial 就失效了。所以按钮回调里
    /// 先用起来，不能延后到下一帧。
    ui::Point last_pointer_{};
    uint32_t  last_serial_{0};

    // 诊断模式状态
    std::string diag_user_;
    std::string diag_pass_;
    std::string diag_shot_;
    std::string diag_scan_dir_;
    float       diag_ms_{0.0f};
    int         diag_stage_{-1};
};
#endif

/// 离屏渲染各页面并存成 PNG。无人值守，用于自动核对排版。
int run_screenshot_mode(const std::string& data_dir, const std::string& out_dir,
                        int width, int height);

}  // namespace penhu::native
