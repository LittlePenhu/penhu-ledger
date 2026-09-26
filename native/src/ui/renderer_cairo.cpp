// =============================================================================
//  native/ui/renderer_cairo.cpp
//  Renderer 的 Linux 实现：Cairo 图元 + Pango 文本 + 自写图片解码。
//
//  和 Windows 那份（renderer.cpp）**逐函数对齐语义**，因为两边的调用方是同一套
//  组件代码。几个必须对齐的点，写在这里免得以后有人以为可以随便改：
//
//  1. **坐标是 DIP**。用 cairo_scale(scale) 把用户空间变成逻辑像素，
//     所有绘制调用拿到的数字和 Windows 侧完全一样。
//     用户空间 = DIP 这件事还顺带解决了 Pango 的字号问题（见下）。
//
//  2. **像素格式对齐 PBGRA**。D2D 用的 GUID_WICPixelFormat32bppPBGRA 是
//     「BGRA + 预乘 alpha」。Cairo 的 CAIRO_FORMAT_ARGB32 在**小端机器上**
//     就是 BGRA 预乘 —— 两者内存布局一致，所以像素拷贝、PNG 导出都能直接复用。
//
//  3. **字号用 pango_font_description_set_absolute_size**，不是 set_size。
//     set_size 的单位是「点」（1/72 英寸），会跟着 cairo 的分辨率设置漂移；
//     set_absolute_size 的单位是**设备单位**，而 pangocairo 的设备单位
//     就是创建 layout 时 cairo 用户空间的单位 —— 也就是我们的 DIP。
//     用错那个是「看起来差不多但每个字号都差几像素」的典型来源。
//
//  4. **行距用设计令牌的确定值**，和 Windows 侧一样。Pango 直到 1.50 才有
//     pango_attr_line_height，Arch 上是 1.58，够用；换算成「行距 / 字号」的比例因子。
//
//  5. **裁剪用 cairo_save/restore 配对**，天然支持嵌套，和 D2D 的
//     Push/PopAxisAlignedClip 语义一致。
// =============================================================================

#include "ui/renderer.hpp"

#include <fontconfig/fontconfig.h>
#include <jpeglib.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <strings.h>   // strcasecmp：POSIX 的，在全局命名空间（std 里没有）
#include <string>
#include <vector>

#include "penhu/util/log.hpp"

namespace penhu::native::ui {

namespace {

/// 界面字体候选链。和 Windows 侧同样的思路：前面几个字形更现代，
/// 最后必须有一个「装了 Arch 就一定有」的兜底。
///
/// Linux 上这个链必须带上 CJK 专用族名（Noto Sans CJK SC）：
/// fontconfig 不会因为你说 "Noto Sans SC" 就自动找到 CJK 变体 ——
/// 名字不一样就是不一样，找不到就掉进 DejaVu，中文全是豆腐块。
const char* kFontCandidates[] = {
    "HarmonyOS Sans SC",
    "Noto Sans SC",
    "Source Han Sans SC",
    "Noto Sans CJK SC",
    "WenQuanYi Micro Hei",
    "Noto Sans",
    "DejaVu Sans",
};

/// 32 位像素：ARGB32（小端 = BGRA 预乘）。
inline uint32_t to_argb(const Color& c) {
    // cairo 的 ARGB32 在内存里是 B,G,R,A（小端），而 uint32_t 字面量
    // 0xAARRGGBB 在内存里是 B,G,R,A 的逆序 —— 所以直接按字节拼，别用位运算。
    return (static_cast<uint32_t>(c.a()) << 24) | (static_cast<uint32_t>(c.r()) << 16) |
           (static_cast<uint32_t>(c.g()) << 8) | static_cast<uint32_t>(c.b());
}

void set_source(cairo_t* cr, const Color& c) {
    cairo_set_source_rgba(cr, static_cast<double>(c.r()) / 255.0,
                          static_cast<double>(c.g()) / 255.0,
                          static_cast<double>(c.b()) / 255.0,
                          static_cast<double>(c.a()) / 255.0);
}

/// 圆角矩形路径。Cairo 没有现成的，用四段圆弧拼。
void rounded_rect_path(cairo_t* cr, const Rect& r, double radius) {
    // 必须先清空当前路径：cairo 的 fill/stroke 不会清路径，save/restore 也不保存路径，
    // 所以上一步画过什么都会留在路径里，被这一步的 cairo_clip / cairo_fill 一起算进去。
    cairo_new_path(cr);
    const double rad = std::max(0.0, std::min(radius,
                                             std::min(static_cast<double>(r.w),
                                                      static_cast<double>(r.h)) * 0.5));
    const double x = r.x, y = r.y, w = r.w, h = r.h;
    if (rad <= 0.01) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    const double pi2 = M_PI / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad,     rad, -pi2, 0.0);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0.0, pi2);
    cairo_arc(cr, x + rad,     y + h - rad, rad, pi2, M_PI);
    cairo_arc(cr, x + rad,     y + rad,     rad, M_PI, M_PI + pi2);
    cairo_close_path(cr);
}

/// 下面这段是「Pango 要求 UTF-8，而界面层到处传 wstring」的唯一转换点。
/// Windows 侧同一件事叫 MultiByteToWideChar，这边没有等价物，就写明了。
std::string to_utf8(const std::wstring& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        // 处理 UTF-16 代理对（emoji 之类的 BMP 外字符）
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            const uint32_t lo = static_cast<uint32_t>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

PangoWeight to_pango_weight(int w) {
    switch (w) {
        case 500: return PANGO_WEIGHT_MEDIUM;
        case 600: return PANGO_WEIGHT_SEMIBOLD;
        case 700: return PANGO_WEIGHT_BOLD;
        default:  return PANGO_WEIGHT_NORMAL;
    }
}

// ---- 从内存读 PNG 的 cairo 回调 -------------------------------------------

struct MemoryReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos{0};
};

cairo_status_t read_from_memory(void* closure, unsigned char* out, unsigned int len) {
    auto* rd = static_cast<MemoryReader*>(closure);
    const size_t remain = (rd->pos < rd->size) ? (rd->size - rd->pos) : 0;
    if (len > remain) return CAIRO_STATUS_READ_ERROR;
    std::memcpy(out, rd->data + rd->pos, len);
    rd->pos += len;
    return CAIRO_STATUS_SUCCESS;
}

// ---- libjpeg 的错误处理 ---------------------------------------------------
//
// libjpeg 的默认错误处理是**直接 exit 进程**。对一个记账软件来说，
// 「打开一张损坏的 JPEG 就把程序干掉」显然不能接受，所以必须把错误接管掉。
// setjmp/longjmp 是它定的接口，不是我们偷懒。

struct JpegError {
    jpeg_error_mgr pub;
    std::jmp_buf    jump;
    char            message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit_cb(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    std::longjmp(err->jump, 1);
}

void jpeg_output_message_cb(j_common_ptr /*cinfo*/) {
    // 警告（比如「文件提前结束」）不打断解码，静默跳过即可
}

/// JPEG 解码到 ARGB32 预乘。和 PNG 那条路返回同一种 surface。
cairo_surface_t* decode_jpeg(const uint8_t* data, size_t size) {
    jpeg_decompress_struct cinfo{};
    JpegError jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_cb;
    jerr.pub.output_message = jpeg_output_message_cb;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        Logger::default_logger().warn(std::string("JPEG 解码失败: ") + jerr.message);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    if (w <= 0 || h <= 0) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    cairo_surface_flush(surf);
    uint8_t* dst = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);

    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* rows[1] = {row.data()};
        const JDIMENSION y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint32_t* line = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * stride);
        for (int x = 0; x < w; ++x) {
            const uint8_t r = row[static_cast<size_t>(x) * 3 + 0];
            const uint8_t g = row[static_cast<size_t>(x) * 3 + 1];
            const uint8_t b = row[static_cast<size_t>(x) * 3 + 2];
            // JPEG 没有 alpha，直接当不透明；ARGB32 是预乘的，a=255 时无需缩放
            line[x] = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) |
                      (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    cairo_surface_mark_dirty(surf);
    return surf;
}

/// 按文件头判断格式再解码。**不看扩展名**：用户把 .png 改名成 .jpg 是常事，
/// 按扩展名分会解不了然后报「格式不支持」，而文件其实是好的。
cairo_surface_t* decode_image_bytes(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 8) return nullptr;

    const bool is_png = data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
    const bool is_jpeg = data[0] == 0xFF && data[1] == 0xD8;

    if (is_png) {
        MemoryReader rd{data, size, 0};
        cairo_surface_t* surf = cairo_image_surface_create_from_png_stream(read_from_memory, &rd);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            Logger::default_logger().warn(
                std::string("PNG 解码失败: ") + cairo_status_to_string(cairo_surface_status(surf)));
            cairo_surface_destroy(surf);
            return nullptr;
        }
        return surf;
    }
    if (is_jpeg) return decode_jpeg(data, size);

    Logger::default_logger().warn("不认识的图片格式（既不是 PNG 也不是 JPEG）");
    return nullptr;
}

}  // namespace

// -----------------------------------------------------------------------------
//  生命周期与目标
// -----------------------------------------------------------------------------

Renderer::~Renderer() { destroy_target(); }

bool Renderer::init() {
    if (fontmap_ != nullptr) return true;

    fontmap_ = pango_cairo_font_map_get_default();
    if (fontmap_ == nullptr) return false;
    // 让 fontconfig 把字体信息读进来。首次调用会扫一遍字体目录，
    // 之后是内存里的缓存 —— 放在 init() 而不是每帧。
    FcInit();

    // 字体只在这里定一次，之后所有字号档共用它。
    // 打进日志是有意的：字体是「看起来对不对」的第一变量。
    family_ = pick_font_family();
    Logger::default_logger().info("界面字体: " + family_);
    return true;
}

std::string Renderer::pick_font_family() const {
    // 问 fontconfig「这个名字能不能真的匹配到一个族」，而不是去猜 ——
    // 装了 HarmonyOS Sans 和没装，在代码里看不出任何区别。
    //
    // 注意这里要**比对匹配结果的名字**：FcFontMatch 永远会返回一个字体
    // （找不到就给默认的），所以「拿到了非空结果」不等于「装了这个字体」。
    // 这个坑很隐蔽 —— 表现是「明明没装 HarmonyOS 却报告在用 HarmonyOS」。
    FcConfig* cfg = FcInitLoadConfigAndFonts();
    if (cfg == nullptr) return "sans";

    for (const char* name : kFontCandidates) {
        FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(name));
        if (pat == nullptr) continue;
        FcConfigSubstitute(cfg, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);

        FcResult result = FcResultNoMatch;
        FcPattern* match = FcFontMatch(cfg, pat, &result);
        bool found = false;
        if (match != nullptr) {
            FcChar8* matched_family = nullptr;
            if (FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
                matched_family != nullptr) {
                found = (strcasecmp(reinterpret_cast<const char*>(matched_family), name) == 0);
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pat);
        if (found) return name;
    }
    return "sans";
}

void Renderer::destroy_target() {
    if (layout_ != nullptr) {
        g_object_unref(layout_);
        layout_ = nullptr;
    }
    if (pctx_ != nullptr) {
        g_object_unref(pctx_);
        pctx_ = nullptr;
    }
    if (cr_ != nullptr) {
        cairo_destroy(cr_);
        cr_ = nullptr;
    }
    if (surf_ != nullptr) {
        cairo_surface_destroy(surf_);
        surf_ = nullptr;
    }
    clip_depth_ = 0;
    cap_w_ = cap_h_ = 0;
    view_w_ = view_h_ = 0;
}

bool Renderer::create_image_target(int width_px, int height_px, float scale) {
    ++target_builds_;   // 与 Windows 侧同口径：构建（含重试）就计一次
    scale_ = scale > 0.0f ? scale : 1.0f;
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);

    surf_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf_) != CAIRO_STATUS_SUCCESS) {
        Logger::default_logger().warn(
            std::string("创建绘图表面失败: ") + cairo_status_to_string(cairo_surface_status(surf_)));
        cairo_surface_destroy(surf_);
        surf_ = nullptr;
        return false;
    }

    cr_ = cairo_create(surf_);
    if (cairo_status(cr_) != CAIRO_STATUS_SUCCESS) {
        destroy_target();
        return false;
    }
    // 用户空间 = DIP。之后所有绘制代码写的就是逻辑像素，
    // 和 Windows 侧（把 DPI 交给 D2D 渲染目标）是同一个效果。
    cairo_scale(cr_, scale_, scale_);
    cairo_set_antialias(cr_, CAIRO_ANTIALIAS_DEFAULT);
    // 线宽按用户空间给（调用方传的是 DIP）；默认的 hairline 在缩放下会变得很细
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);

    pctx_ = pango_cairo_create_context(cr_);
    if (pctx_ == nullptr) {
        destroy_target();
        return false;
    }
    layout_ = pango_layout_new(pctx_);
    if (layout_ == nullptr) {
        destroy_target();
        return false;
    }

    cap_w_ = w;
    cap_h_ = h;
    return true;
}

bool Renderer::attach_window(PlatformWindow /*surface*/, float scale) {
    // Linux 侧拿不到窗口尺寸：Wayland 的尺寸是合成器通过 xdg_toplevel.configure
    // 告诉客户端的，而那件事只有 shell（事件循环）知道。
    // 所以这里先建一个 1x1 的目标，shell 收到 configure 后调用 resize() 定尺寸。
    // 保留这个入口（而不是让 shell 直接调 attach_offscreen）是为了两平台接口一致：
    // 「窗口模式 / 离屏模式」这个区分在两边都成立，只是 Linux 的窗口尺寸晚一步知道。
    destroy_target();
    offscreen_ = false;
    return create_image_target(1, 1, scale);
}

bool Renderer::attach_offscreen(int width_px, int height_px, float scale) {
    destroy_target();
    offscreen_ = true;
    if (!create_image_target(width_px, height_px, scale)) return false;
    view_w_ = cap_w_;
    view_h_ = cap_h_;
    logical_ = {static_cast<float>(view_w_) / scale_, static_cast<float>(view_h_) / scale_};
    return true;
}

void Renderer::detach() { destroy_target(); }

bool Renderer::resize(int width_px, int height_px, bool allow_reclaim) {
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);
    if (w == view_w_ && h == view_h_ && cr_ != nullptr) return true;

    // 与 Windows 侧同一套「容量带余量 + 半容量滞回回收」策略。
    // 旧实现每次都精确重建，理由是「cairo 的 image surface 只是一次 malloc」——
    // 但 destroy_target 还会连带销毁 Pango 上下文与 layout：合成器拖拽缩放的
    // configure 风暴里这是每帧一次的布局对象重建，卡顿就来自这里。
    // 内存代价由「缩到容量一半才回收」兜住（与 Windows 侧同款滞回）。
    const float keep_scale = scale_;
    view_w_ = w;
    view_h_ = h;
    logical_ = {static_cast<float>(w) / scale_, static_cast<float>(h) / scale_};

    if (cr_ != nullptr && w <= cap_w_ && h <= cap_h_) {
        // 尺寸连续变化进行中只增不减：回收重建落在某一帧上就是一次卡顿。
        if (!allow_reclaim) return true;
        if (w >= cap_w_ / 2 && h >= cap_h_ / 2) return true;
    }

    // 余量口径与 Windows 侧一致：宽度的 1/4（下限 384、上限 1024）。
    const int nw = w + std::clamp(w / 4, 384, 1024);
    const int nh = h + std::clamp(h / 4, 384, 1024);
    // cairo 版 destroy_target 会把 view/cap 一并清零，重建后要放回去。
    destroy_target();
    if (!create_image_target(nw, nh, keep_scale)) return false;
    view_w_ = w;
    view_h_ = h;
    logical_ = {static_cast<float>(w) / scale_, static_cast<float>(h) / scale_};
    return true;
}

bool Renderer::reserve(int width_px, int height_px) {
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);
    if (cr_ != nullptr && w <= cap_w_ && h <= cap_h_) return true;
    // 只扩容量，不动视口/逻辑尺寸（与 Windows 侧语义一致）。
    // Linux 没有自驱动的窗口动画，这个入口用于接口对齐，也顺带覆盖
    // 「configure 给出的尺寸突变」这类一次配到位的场景。
    const int keep_w = view_w_, keep_h = view_h_;
    const Size keep_logical = logical_;
    const float keep_scale = scale_;
    destroy_target();
    if (!create_image_target(w, h, keep_scale)) return false;
    view_w_ = keep_w;
    view_h_ = keep_h;
    logical_ = keep_logical;
    return true;
}

const uint8_t* Renderer::pixels(int* stride_px) const {
    if (surf_ == nullptr) return nullptr;
    cairo_surface_flush(surf_);
    if (stride_px != nullptr) {
        // cairo 的 stride 是字节数；换成像素数让调用方少一次除法
        *stride_px = cairo_image_surface_get_stride(surf_) / 4;
    }
    return cairo_image_surface_get_data(surf_);
}

void Renderer::present() {
    // Linux 上是空操作：Wayland 没有「往窗口 DC 里贴一张位图」这回事，
    // 必须由外部把像素拷进 wl_shm buffer 再 attach + commit（协议要求这么走）。
    // 那份工作在 shell_linux.cpp 里，因为只有它持有 wl_surface / wl_shm。
    //
    // 这样切分的好处：渲染器完全不依赖 Wayland 类型，离屏截图和窗口显示
    // 走的是同一条绘制路径 —— 截图里看到的和屏幕上的必然一致。
}

// -----------------------------------------------------------------------------
//  帧
// -----------------------------------------------------------------------------

bool Renderer::begin_frame(const Color& clear) {
    if (cr_ == nullptr) return false;

    // 清屏单独用一段 save/restore：identity 变换让 paint 覆盖整张表面
    // （否则只覆盖缩放后的逻辑区域，右下角会留脏数据）。
    cairo_save(cr_);
    cairo_identity_matrix(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
    set_source(cr_, clear);
    cairo_paint(cr_);
    cairo_restore(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_OVER);

    clip_depth_ = 0;
    return true;
}

bool Renderer::end_frame() {
    if (cr_ == nullptr) return false;
    // 把可能的裁剪栈收干净：调用方漏配对 pop_clip 时，
    // 下一帧的 begin_frame 会自己重置 clip_depth_，但 cairo 的状态栈不会自动清。
    while (clip_depth_ > 0) {
        cairo_restore(cr_);
        --clip_depth_;
    }
    cairo_surface_flush(surf_);
    return true;
}

// -----------------------------------------------------------------------------
//  图元
// -----------------------------------------------------------------------------

void Renderer::fill(const Rect& r, const Color& c) {
    if (cr_ == nullptr || r.empty() || c.a() == 0) return;
    set_source(cr_, c);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
    cairo_fill(cr_);
}

void Renderer::fill_round(const Rect& r, float radius, const Color& c) {
    if (cr_ == nullptr || r.empty() || c.a() == 0) return;
    set_source(cr_, c);
    rounded_rect_path(cr_, r, radius);
    cairo_fill(cr_);
}

void Renderer::stroke_round(const Rect& r, float radius, const Color& c, float width) {
    if (cr_ == nullptr || r.empty() || c.a() == 0) return;
    set_source(cr_, c);
    // 描边以路径为中心，半宽会溢出到矩形外。这里按半宽内缩，
    // 让「边框刚好落在给定的矩形里」—— 和 Windows 侧同一处理，
    // 否则相邻卡片的描边会互相压住。
    const float half = width * 0.5f;
    const Rect rr{r.x + half, r.y + half, r.w - width, r.h - width};
    if (rr.empty()) return;
    rounded_rect_path(cr_, rr, std::max(0.0f, radius - half));
    cairo_set_line_width(cr_, width);
    cairo_stroke(cr_);
}

void Renderer::fill_circle(float cx, float cy, float radius, const Color& c) {
    if (cr_ == nullptr || radius <= 0.0f || c.a() == 0) return;
    set_source(cr_, c);
    cairo_new_path(cr_);
    cairo_arc(cr_, cx, cy, radius, 0.0, 2.0 * M_PI);
    cairo_fill(cr_);
}

void Renderer::draw_line(const Point& a, const Point& b, const Color& c, float width) {
    if (cr_ == nullptr || c.a() == 0) return;
    set_source(cr_, c);
    cairo_set_line_width(cr_, width);
    cairo_new_path(cr_);
    cairo_move_to(cr_, a.x, a.y);
    cairo_line_to(cr_, b.x, b.y);
    cairo_stroke(cr_);
}

void Renderer::push_clip(const Rect& r) {
    if (cr_ == nullptr) return;
    // 每压一层裁剪就对应一个 cairo 状态栈条目，pop 时 restore 回去。
    // 天然支持嵌套，也不需要自己维护矩形交集。
    cairo_save(cr_);
    cairo_new_path(cr_);   // 同 rounded_rect_path 的理由：不清路径会把旧图元一起裁进去
    cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
    cairo_clip(cr_);
    ++clip_depth_;
}

void Renderer::pop_clip() {
    if (cr_ == nullptr || clip_depth_ <= 0) return;
    cairo_restore(cr_);
    --clip_depth_;
}

// -----------------------------------------------------------------------------
//  文本
// -----------------------------------------------------------------------------

namespace {

/// 把 TypeToken 的「字号 + 行距 + 字重」套到 Pango 上。
/// 行距用 pango_attr_line_height（Pango ≥1.50）按**比例因子**给：
/// 比例 = 行距 / 字号，这样字号一变行距自动跟着走，和设计令牌的意图一致。
void apply_type(PangoLayout* layout, const TypeToken& tok, const std::string& family,
                TextAlign ha) {
    PangoAttrList* attrs = pango_attr_list_new();
    const double factor = (tok.size > 0.5f) ? (static_cast<double>(tok.line_height) /
                                               static_cast<double>(tok.size))
                                            : 1.2;
    pango_attr_list_insert(attrs, pango_attr_line_height_new(factor));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);

    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, family.c_str());
    // 单位是**设备单位**（= 我们的 DIP），不是点。见文件头第 3 条。
    pango_font_description_set_absolute_size(desc, static_cast<double>(tok.size) * PANGO_SCALE);
    pango_font_description_set_weight(desc, to_pango_weight(tok.weight));
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    switch (ha) {
        case TextAlign::Center: pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER); break;
        case TextAlign::Right:  pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);  break;
        default:                pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);   break;
    }
}

}  // namespace

Size Renderer::measure(const std::wstring& text, TypeStyle style, float max_width, bool wrap) const {
    if (cr_ == nullptr || layout_ == nullptr || text.empty()) return {0.0f, 0.0f};

    const std::string utf8 = to_utf8(text);
    pango_layout_set_text(layout_, utf8.c_str(), static_cast<int>(utf8.size()));
    const TypeToken tok = Theme::light().type(style);
    apply_type(layout_, tok, family_, TextAlign::Left);

    if (wrap) {
        pango_layout_set_width(layout_, static_cast<int>(std::max(1.0f, max_width) * PANGO_SCALE));
        pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
    } else {
        // 不换行时宽度设 -1（无约束），这样量出来的是文本的真实宽度
        pango_layout_set_width(layout_, -1);
    }
    pango_layout_set_ellipsize(layout_, PANGO_ELLIPSIZE_NONE);
    pango_cairo_update_layout(cr_, layout_);

    PangoRectangle logical{};
    pango_layout_get_pixel_extents(layout_, nullptr, &logical);
    return {static_cast<float>(logical.width), static_cast<float>(logical.height)};
}

void Renderer::text(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
                    TextAlign ha, VAlign va, bool wrap, bool ellipsis) {
    if (cr_ == nullptr || layout_ == nullptr || s.empty() || box.empty()) return;

    const std::string utf8 = to_utf8(s);
    pango_layout_set_text(layout_, utf8.c_str(), static_cast<int>(utf8.size()));
    const TypeToken tok = Theme::light().type(style);
    apply_type(layout_, tok, family_, ha);

    if (wrap || ellipsis) {
        pango_layout_set_width(layout_, static_cast<int>(std::max(1.0f, box.w) * PANGO_SCALE));
        pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
    } else {
        pango_layout_set_width(layout_, -1);
    }
    pango_layout_set_ellipsize(layout_,
                               ellipsis ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
    // 高度也要限制：单行框里塞多行文本时，Pango 默认会把框撑开画出去。
    // Windows 侧靠 D2D 的 DrawTextLayout + D2D1_DRAW_TEXT_OPTIONS_CLIP 达到同样效果。
    //
    // ⚠️ 单位陷阱：pango_layout_set_height 的**正负号改变单位** ——
    // 正数是「高度，单位 Pango 单位（= 1/1024 像素）」，负数是「行数」。
    // 第一版写成 `-box.h * PANGO_SCALE`（看着像「不超过这么高」），
    // 实际含义变成「省略到 box.h*1024 行」—— 一个天文数字的行数限制
    // 会让整段文字一行都不画出来，现象是**整个界面一个字都没有**，
    // 而布局、色块、圆角全都正常，非常容易被误判成「字体没装」。
    pango_layout_set_height(layout_, static_cast<int>(std::max(1.0f, box.h) * PANGO_SCALE));
    pango_cairo_update_layout(cr_, layout_);

    PangoRectangle logical{};
    pango_layout_get_pixel_extents(layout_, nullptr, &logical);

    float y = box.y;
    if (va == VAlign::Middle) y = box.y + (box.h - static_cast<float>(logical.height)) * 0.5f;
    else if (va == VAlign::Bottom) y = box.bottom() - static_cast<float>(logical.height);

    // ⚠️ 顺序不能改：**先建裁剪路径，再设置当前点**。
    //
    // cairo_new_path() 除了清路径，还会**把当前点重置为 (0,0)** ——
    // 这是 cairo 里最容易忽略的一条语义。原来的写法是
    //     move_to(box.x, y) → save → new_path → rectangle → clip → show_layout
    // 看起来没问题，实际 new_path 把刚设好的落点清成了 (0,0)，
    // 于是文字被画到画布左上角、又立刻被裁剪区裁掉 ——
    // **一个字都不显示**，而布局、色块、圆角、命中测试全部正常。
    // 这一处排查了很久，因为所有中间量（布局尺寸、颜色、cairo_status）都是对的，
    // 唯独「画在哪」被悄悄改了。
    cairo_save(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, box.x, box.y, box.w, box.h);
    cairo_clip(cr_);

    set_source(cr_, c);
    cairo_move_to(cr_, box.x, y);
    pango_cairo_show_layout(cr_, layout_);
    cairo_restore(cr_);
}

void Renderer::text_line(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
                         TextAlign ha, bool ellipsis) {
    text(s, box, style, c, ha, VAlign::Middle, false, ellipsis);
}

// -----------------------------------------------------------------------------
//  PNG 导出
// -----------------------------------------------------------------------------

bool Renderer::save_png(const std::string& utf8_path) {
    if (surf_ == nullptr) return false;

    // 只导出**视口**那块。窗口模式下表面可能比视口大（resize 之前的旧尺寸），
    // 整张导出会多出一段从没画过的区域 —— 和 Windows 侧同一处理。
    const int w = view_w_ > 0 ? view_w_ : cap_w_;
    const int h = view_h_ > 0 ? view_h_ : cap_h_;
    if (w <= 0 || h <= 0) return false;

    cairo_surface_t* view = cairo_surface_create_for_rectangle(
        surf_, 0.0, 0.0, static_cast<double>(w), static_cast<double>(h));
    const cairo_status_t st = cairo_surface_write_to_png(view, utf8_path.c_str());
    cairo_surface_destroy(view);
    if (st != CAIRO_STATUS_SUCCESS) {
        Logger::default_logger().warn(
            std::string("PNG 写入失败: ") + cairo_status_to_string(st) + " → " + utf8_path);
        return false;
    }
    return true;
}

bool Renderer::save_screen_png(const std::string& /*utf8_path*/) {
    // Wayland 的协议里**没有**「抓整屏」这个能力，这是有意设计的（隐私）。
    // 需要抓屏得走 xdg-desktop-portal 的 Screenshot 接口，那要用户在弹出的
    // 对话框里点确认 —— 对一个无人值守的诊断模式来说没有意义。
    //
    // 影响：Windows 侧那条「任务栏可见性」断言在 Linux 上做不了。
    // 好在 Linux 没有「最大化时任务栏被涂黑」那个问题（Wayland 里窗口尺寸
    // 由合成器决定，客户端根本没机会把矩形外扩到屏幕外），所以也不需要它。
    Logger::default_logger().warn("save_screen_png 在 Linux 上不可用（Wayland 不允许客户端抓屏）");
    return false;
}

// -----------------------------------------------------------------------------
//  图片
// -----------------------------------------------------------------------------

bool Renderer::load_image(const std::wstring& path, BitmapRef& out) const {
    const std::string utf8 = to_utf8(path);
    FILE* f = std::fopen(utf8.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return false;

    cairo_surface_t* surf = decode_image_bytes(bytes.data(), bytes.size());
    if (surf == nullptr) return false;
    out = bitmap_take(surf);
    return true;
}

bool Renderer::load_image_mem(const void* data, size_t bytes, BitmapRef& out) const {
    if (data == nullptr || bytes == 0) return false;
    cairo_surface_t* surf =
        decode_image_bytes(static_cast<const uint8_t*>(data), bytes);
    if (surf == nullptr) return false;
    out = bitmap_take(surf);
    return true;
}

void Renderer::draw_image(RawBitmap* src, const Rect& box, float radius, bool cover) {
    if (cr_ == nullptr || src == nullptr || box.empty()) return;

    const int iw = cairo_image_surface_get_width(src);
    const int ih = cairo_image_surface_get_height(src);
    if (iw <= 0 || ih <= 0) return;
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) return;

    const float sw = static_cast<float>(iw);
    const float sh = static_cast<float>(ih);

    // 和 Windows 侧同一套缩放规则（cover = 按短边填满，否则等比内fit）
    const float scale = cover ? std::max(box.w / sw, box.h / sh)
                              : std::min(box.w / sw, box.h / sh);
    const float dw = sw * scale;
    const float dh = sh * scale;
    const Rect dst{box.x + (box.w - dw) * 0.5f, box.y + (box.h - dh) * 0.5f, dw, dh};

    cairo_save(cr_);
    // 这里用**圆角**裁剪。
    //
    // Windows 侧同样的 radius 参数只做了矩形裁剪（push_clip(box)），
    // 也就是说 `Theme::kCornerSm` 传下去其实没生效 —— 那里是个 bug，
    // 记在 docs/LINUX_VERIFY.md 里待修。Linux 这边按参数的本意做圆的，
    // 所以两边截图在图片四角会有肉眼可见的差别（这一条是已知的、有意的差异）。
    if (radius > 0.01f) {
        rounded_rect_path(cr_, box, radius);
        cairo_clip(cr_);
    }
    cairo_translate(cr_, dst.x, dst.y);
    cairo_scale(cr_, dst.w / sw, dst.h / sh);
    cairo_set_source_surface(cr_, src, 0.0, 0.0);
    // 缩略图一定要双线性：最近邻在大幅缩小时会出现明显的锯齿，
    // 而这张图是给用户看「是不是这一张」用的。
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
}

}  // namespace penhu::native::ui
