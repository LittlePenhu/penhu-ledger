#pragma once
// =============================================================================
//  native/ui/renderer.hpp
//  绘制后端的**平台无关接口**。
//
//  两条设计线，都是为了后面能少踩坑：
//
//  1) **同一套绘制代码，两种渲染目标**
//     · 窗口目标       —— 窗口里正常显示
//     · 离屏位图目标   —— 离屏渲染成 PNG
//     两者共用同一套 fill/round/text 调用。离屏那条路是刻意留的：
//     界面排版好不好看只有看到图才知道，而「把窗口截下来」依赖真实桌面会话；
//     离屏渲染不依赖，可以无人值守跑。
//
//  2) **坐标一律 DIP，缩放交给渲染目标**
//     后端把缩放设成 scale，于是所有绘制坐标都是「逻辑像素」，
//     布局代码在 100% / 125% / 150% 缩放的屏上写同一套数字。
//
//  ---------------------------------------------------------------------------
//  平台差异怎么放（2026-09-22 移植 Linux 时加的）
//
//  公开接口完全一致，差异只有两处，都在下面用 #ifdef 分开了：
//    · PlatformWindow    —— 窗口句柄（Windows 是 HWND，Linux 是 wl_surface*）
//    · private 成员与私有辅助函数 —— 一个是 D2D/DWrite/WIC，一个是 Cairo/Pango
//
//  为什么不做成 pimpl 把私有成员也藏起来：renderer.cpp 里有近两百处成员引用，
//  为了「头文件更干净」去动它们，收益远小于风险。私有成员本来就是实现细节，
//  用 #ifdef 分开同样达到「Linux 编译时看不到 D2D 类型」的目的。
//  ---------------------------------------------------------------------------
// =============================================================================

#include "ui/bitmap.hpp"
#include "ui/foundation.hpp"

#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#else
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

// 只用到指针，不需要把 Wayland 的头拖进每一个包含 renderer 的编译单元
struct wl_surface;
struct wl_display;
#endif

#include <string>
#include <unordered_map>

namespace penhu::native::ui {

#ifdef _WIN32
using PlatformWindow = HWND;
/// ComPtr 必须在**命名空间作用域**引入 —— `using` 声明不能写在类体里
/// （MSVC 会报 C2886）。Linux 侧没有这个概念，所以整条只在 Windows 下存在。
using Microsoft::WRL::ComPtr;
#else
using PlatformWindow = wl_surface*;
#endif

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// 建后端的全局对象（Windows：D2D/DWrite/WIC 三个工厂；Linux：字体映射表）。
    /// 进程内调用一次即可。
    bool init();

    /// 绑定窗口目标。scale = 窗口 DPI / 96。
    /// Windows 侧内部**同样是离屏软件渲染**，画完由 present() 贴到窗口上 ——
    /// 为什么不用 ID2D1HwndRenderTarget，见 .cpp 里那段说明（真实崩溃换来的）。
    bool attach_window(PlatformWindow w, float scale);
    /// 绑定离屏位图目标（截图用）。尺寸是**物理像素**。
    bool attach_offscreen(int width_px, int height_px, float scale);
    /// 把刚画完的一帧交给后端呈现。离屏模式是空操作。
    ///
    /// Windows 是 SetDIBitsToDevice 贴到窗口 DC；
    /// Linux 是把 cairo image surface 的内容拷进一块 wl_shm buffer、
    /// attach + commit（Wayland 没有「直接写窗口」这回事）。
    void present();
    void detach();

    /// 改视口尺寸（物理像素）。容量带余量复用，装得下就不重建渲染目标。
    /// allow_reclaim=false 表示「尺寸连续变化进行中」（窗口动画 / 拖拽缩放）：
    /// 此时只增不减 —— 在运动中途为回收内存而重建渲染目标，会在某一帧上
    /// 表现为突然卡顿。等运动结束后的自然尺寸变化再回收也不迟。
    bool resize(int width_px, int height_px, bool allow_reclaim = true);

    /// 预留容量：确保渲染目标**至少**覆盖 w×h，不动视口/逻辑尺寸。
    /// 窗口动画开始前调它一次（按全程最大尺寸），动画中逐帧的 resize() 就
    /// 全程复用同一块位图 —— 否则容量不足的帧要 destroy+create 一次
    /// 16MB 级的 WIC 位图 + D2D 软件渲染目标，那一帧必然掉帧。
    bool reserve(int width_px, int height_px);

    /// 渲染目标累计构建次数（含失败重试）。诊断用：窗口动画前后取样一减，
    /// 就知道动画途中有没有发生渲染目标重建（预分配生效时必须是 0）。
    int target_builds() const { return target_builds_; }

    bool begin_frame(const Color& clear);
    /// 返回 false 表示后端要求重建目标（D2D 设备丢失），调用方应重新 attach。
    /// Linux 侧恒返回 true —— Wayland 的 shm buffer 不会被「设备」夺走。
    bool end_frame();

    /// 把离屏目标存成 PNG。只有离屏模式可用。
    bool save_png(const std::string& utf8_path);

    /// 逻辑尺寸（DIP）。布局代码用它。
    Size logical_size() const { return logical_; }
    /// 渲染目标是否已就绪。
    /// 窗口创建过程中系统会先发一次尺寸变化，那时还没 attach 好 ——
    /// 没有这个判断的话那一次会去画一个 0x0 的目标（虽不崩，但是无意义的工作）。
    bool has_target() const {
#ifdef _WIN32
        return rt_ != nullptr;
#else
        return cr_ != nullptr;
#endif
    }
    float scale() const { return scale_; }

    // ---- 图元 ----
    void fill(const Rect& r, const Color& c);
    void fill_round(const Rect& r, float radius, const Color& c);
    void stroke_round(const Rect& r, float radius, const Color& c, float width = 1.0f);
    void fill_circle(float cx, float cy, float radius, const Color& c);
    void draw_line(const Point& a, const Point& b, const Color& c, float width = 1.0f);

    // ---- 裁剪（栈式，支持嵌套）----
    void push_clip(const Rect& r);
    void pop_clip();

    // ---- 文本 ----
    /// 测量。max_width 用于换行；wrap=false 时按不换行测（返回真实宽度）
    Size measure(const std::wstring& text, TypeStyle style, float max_width, bool wrap = true) const;

    void text(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
              TextAlign ha = TextAlign::Left, VAlign va = VAlign::Top,
              bool wrap = true, bool ellipsis = false);

    /// 便捷：单行、垂直居中
    void text_line(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
                   TextAlign ha = TextAlign::Left, bool ellipsis = false);

    // ---- 图片 ----
    /// 从磁盘解码（Windows：WIC；Linux：cairo 的 PNG 读入 / 自己写的 JPEG 路径）
    bool load_image(const std::wstring& path, BitmapRef& out) const;
    /// 从内存解码 PNG。顶栏 logo 存在资源里（Windows：RCDATA；
    /// Linux：编译进二进制的字节数组），没有文件路径可给。
    bool load_image_mem(const void* data, size_t bytes, BitmapRef& out) const;
    /// 把**整块屏幕**存成 PNG。只有 Windows 的「任务栏可见」诊断需要它 ——
    /// 离屏截图只含我们的客户区，窗口外的部分根本不在图里。
    /// Linux 侧返回 false（Wayland 不允许客户端随便抓屏，这是协议设计）。
    bool save_screen_png(const std::string& utf8_path);
    /// cover=true 按短边填满并居中裁剪（缩略图用），false 则等比内fit
    void draw_image(RawBitmap* src, const Rect& box, float radius, bool cover);

#ifndef _WIN32
    // ---- 仅 Linux：把当前帧的像素交给外部上传 ----
    //
    // Wayland 没有「往窗口画」这回事：客户端要把像素写进一块 wl_shm 共享内存
    // buffer、attach 给 wl_surface、再 commit —— 这几步必须由持有 wl_shm 的
    // 那一方做。为了不让渲染器依赖 Wayland 类型，把这块留在 shell 里：
    // shell 调 pixels() 拿到内存，自己走协议。
    //
    // 这样切分还顺带保证了一件事：窗口显示和离屏截图用的是同一份像素，
    // 截图里看到的和屏幕上的必然一致。
    const uint8_t* pixels(int* stride_px) const;
    int  pixel_width() const { return view_w_ > 0 ? view_w_ : cap_w_; }
    int  pixel_height() const { return view_h_ > 0 ? view_h_ : cap_h_; }
#endif

private:
#ifdef _WIN32
    // =========================================================================
    //  Windows：Direct2D + DirectWrite + WIC
    // =========================================================================
    bool encode_png(RawBitmap* src, UINT w, UINT h, const std::wstring& path);
    IDWriteTextFormat* format_for(TypeStyle style, TextAlign ha, bool wrap) const;
    /// 按优先级从**系统已安装**的字体里挑一个界面字体（见 .cpp 里的候选链）
    std::wstring pick_font_family() const;
    D2D1_COLOR_F to_d2d(const Color& c) const;
    void destroy_target();
    /// 建立「软件光栅器 + WIC 位图」这个渲染目标。窗口与离屏两种模式共用。
    bool create_software_target(int width_px, int height_px, float scale);

    ComPtr<ID2D1Factory>          d2d_;
    ComPtr<IDWriteFactory>        dwrite_;
    ComPtr<IWICImagingFactory>    wic_;

    ComPtr<ID2D1RenderTarget>     rt_;
    ComPtr<IWICBitmap>            wic_bitmap_;
    ComPtr<ID2D1SolidColorBrush>  brush_;
    /// 非空表示这个渲染器在给某个窗口画，present() 时会贴到它上面
    HWND                          hwnd_{nullptr};

    /// 实际选中的界面字体族。不是编译期常量 —— 由这台机器上装了哪些字体决定。
    std::wstring                  font_family_{L"Microsoft YaHei UI"};

    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> formats_;
    int   clip_depth_{0};

    /// 位图的**容量**尺寸与当前**视口**尺寸（单位都是物理像素）。
    ///
    /// 两者分开是为了让 resize 不必重建位图。原来每来一次 WM_SIZE 就
    /// destroy + create 一张整窗口大小的 WIC 位图和一个 D2D 渲染目标
    /// （2560x1600 约 16MB），而拖拽窗口边缘时 WM_SIZE 是每移动一下就来一次 ——
    /// 这一步是「缩放卡顿、进而露出背景色闪白」的主要来源。
    ///
    /// 现在的规则：容量只按需增长（并带一点余量），视口直接改；
    /// 只有当视口缩到容量一半以下时才回收，避免「拖大拖小」来回重建。
    ///
    /// ↓ 这四个字段两个平台共用（Linux 只是不用「容量带余量」那套），
    ///   所以声明在下面的公共私有区，不在这段 Windows 专属里。

#else
    // =========================================================================
    //  Linux：Cairo + Pango
    //
    //  为什么不是 GL/Vulkan：整个界面是软件绘制的（Windows 侧也是软件光栅器），
    //  用 image surface + wl_shm 上传共享内存就能显示，不碰 GPU。
    //  WSLg 的 GPU 透传时好时坏，绕开它反而更稳；代价是大窗口下 CPU 占用高一些。
    // =========================================================================
    /// 建一张给定物理尺寸的 ARGB32 表面，并把用户空间设成 DIP
    bool create_image_target(int width_px, int height_px, float scale);
    /// 按候选链问 fontconfig「这个名字有没有真的装上」
    std::string pick_font_family() const;
    void destroy_target();

    cairo_surface_t* surf_{nullptr};
    cairo_t*         cr_{nullptr};
    /// Pango 的上下文/布局都挂在当前 cairo 目标上，换目标就得重建
    PangoContext*    pctx_{nullptr};
    PangoFontMap*    fontmap_{nullptr};
    /// 复用一个 layout：重建 layout 要重新解析字体，每帧几百次不划算。
    /// mutable 是因为 measure() 是 const 但也要用它量文本。
    mutable PangoLayout* layout_{nullptr};
    std::string      family_{"sans"};
    int              clip_depth_{0};
    /// 窗口模式 / 离屏模式。Linux 上两者的绘制路径完全相同，
    /// 只在 save_png 的取景范围上有区别，所以只记一个标志。
    bool             offscreen_{false};
#endif

    /// 视口与容量（物理像素）。两平台共用，见上面那段说明。
    int   cap_w_{0};
    int   cap_h_{0};
    int   view_w_{0};
    int   view_h_{0};

    Size  logical_{};
    float scale_{1.0f};
    /// create_*_target 的累计调用次数（含失败重试），见 target_builds()
    int   target_builds_{0};
};

}  // namespace penhu::native::ui
