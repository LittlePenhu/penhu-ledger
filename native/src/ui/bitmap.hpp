#pragma once
// =============================================================================
//  native/ui/bitmap.hpp
//  平台无关的位图句柄。
//
//  为什么要有这个文件：Windows 侧的解码结果是 WIC 的 IWICBitmapSource（COM 引用
//  计数），Linux 侧是 Cairo 的 cairo_surface_t（自带引用计数）。而页面代码
//  （scan 的图片预览、titlebar 的 logo）只是「拿着一个句柄传给 draw_image」，
//  不该知道背后是哪一种。原来这几处直接写 ComPtr<IWICBitmapSource> ——
//  那行在 Linux 上根本编译不过，所以把句柄抽到这里。
//
//  两个平台都要提供三个东西，缺一不可：
//    RawBitmap    —— 传给 draw_image 的裸指针类型
//    BitmapRef    —— 持有者（自动释放）
//    bitmap_raw() —— 从持有者取出裸指针
//    bitmap_ok()  —— 判断是否有效（避免各处写 `!= nullptr` 或 `.Get()`）
// =============================================================================

#ifdef _WIN32

#include <wincodec.h>
#include <wrl/client.h>

namespace penhu::native::ui {

using RawBitmap = IWICBitmapSource;
using BitmapRef = Microsoft::WRL::ComPtr<IWICBitmapSource>;

inline RawBitmap* bitmap_raw(const BitmapRef& b) { return b.Get(); }
inline bool       bitmap_ok(const BitmapRef& b) { return b != nullptr; }

}  // namespace penhu::native::ui

#else

#include <cairo/cairo.h>

#include <memory>

namespace penhu::native::ui {

using RawBitmap = cairo_surface_t;
/// cairo_surface_t 自带引用计数，但接口是 C 风格的手工管理。
/// 这里用 shared_ptr 包一层，语义与 Windows 侧的 ComPtr 对齐
/// （拷贝即共享，最后一个引用销毁时释放）。
using BitmapRef = std::shared_ptr<cairo_surface_t>;

inline RawBitmap* bitmap_raw(const BitmapRef& b) { return b.get(); }
inline bool       bitmap_ok(const BitmapRef& b) { return b != nullptr; }

/// 把「已经持有一份引用」的 cairo surface 交给 BitmapRef 接管。
/// 调用约定：传进来之后不要再自己 destroy —— 所有权转移。
inline BitmapRef bitmap_take(cairo_surface_t* s) {
    return BitmapRef(s, [](cairo_surface_t* p) {
        if (p != nullptr) cairo_surface_destroy(p);
    });
}

}  // namespace penhu::native::ui

#endif
