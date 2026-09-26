// =============================================================================
//  native/ui/renderer.cpp
// =============================================================================

#include "ui/renderer.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "penhu/util/log.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace penhu::native::ui {

namespace {
/// 界面字体的候选链，按优先级排列。
///
/// 为什么用链而不是写死一个：前几个（HarmonyOS Sans SC / Noto Sans SC /
/// 思源黑体）字形更现代 —— 笔画更匀、小字号下更清楚，中英混排的基线更整齐。
/// 但它们**不是 Windows 自带的**，装了就有、没装就没有。
/// 所以顺序是「有就用更好的，没有就退回一定存在的」：
/// 最后三档是 Windows 中文环境的保底，永远不会全部落空。
///
/// 挑哪一个不是靠猜：由 DirectWrite 的系统字体集合回答（见 pick_font_family）。
const wchar_t* kFontCandidates[] = {
    L"HarmonyOS Sans SC",
    L"Noto Sans SC",
    L"Source Han Sans SC",
    L"Microsoft YaHei UI",
    L"Microsoft YaHei",
    L"Segoe UI",
};
/// 全都找不到时的兜底（实际上不会发生：雅黑是 Windows 中文的必备字体）
const wchar_t* kFontFallback = L"Microsoft YaHei UI";

DWRITE_FONT_WEIGHT to_weight(int w) {
    switch (w) {
        case 500: return DWRITE_FONT_WEIGHT_MEDIUM;
        case 600: return DWRITE_FONT_WEIGHT_SEMI_BOLD;
        case 700: return DWRITE_FONT_WEIGHT_BOLD;
        default:  return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}
}  // namespace

Renderer::~Renderer() { destroy_target(); }

bool Renderer::init() {
    if (d2d_ != nullptr) return true;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic_.GetAddressOf())))) {
        return false;
    }

    // 字体只在这里定一次，之后所有字号档共用它。
    // 打进日志是有意的：字体是「看起来对不对」的第一变量 ——
    // 将来有人问「怎么和上次截图不一样」，先看这一行。
    font_family_ = pick_font_family();
    Logger::default_logger().info(
        "界面字体: " + std::string(font_family_.begin(), font_family_.end()));
    return true;
}

std::wstring Renderer::pick_font_family() const {
    if (dwrite_ == nullptr) return kFontFallback;

    // 问系统「这个族名存在吗」，而不是去猜 ——
    // 装了 HarmonyOS Sans 和没装，在代码里看不出任何区别，只有字体集合知道。
    ComPtr<IDWriteFontCollection> coll;
    if (FAILED(dwrite_->GetSystemFontCollection(coll.GetAddressOf(), FALSE)) ||
        coll == nullptr) {
        return kFontFallback;
    }
    for (const wchar_t* name : kFontCandidates) {
        UINT32 index = 0;
        BOOL   exists = FALSE;
        if (SUCCEEDED(coll->FindFamilyName(name, &index, &exists)) && exists) {
            return name;
        }
    }
    return kFontFallback;
}

void Renderer::destroy_target() {
    clip_depth_ = 0;
    brush_.Reset();
    rt_.Reset();
    wic_bitmap_.Reset();
    // 注意：不清 hwnd_ —— resize 与重新 attach 时都要靠它判断当前是窗口模式
}

bool Renderer::create_software_target(int width_px, int height_px, float scale) {
    ++target_builds_;   // 含失败重试：失败后每帧重试也会被计到，正好暴露问题
    scale_ = scale > 0.0f ? scale : 1.0f;
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);

    if (FAILED(wic_->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h),
                                  GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                  wic_bitmap_.GetAddressOf()))) {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = 96.0f * scale_;
    props.dpiY = 96.0f * scale_;
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    // 软件光栅器：不依赖显卡，也不受远程会话 / 无 GPU 环境影响。
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;

    if (FAILED(d2d_->CreateWicBitmapRenderTarget(wic_bitmap_.Get(), props, rt_.GetAddressOf()))) {
        return false;
    }
    cap_w_ = w;
    cap_h_ = h;
    // 注意：这里**不**设置 logical_。容量带余量，不等于视口；
    // logical_ 由调用方按视口尺寸设置（见 attach_* 与 resize）。

    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush_.GetAddressOf()))) {
        return false;
    }
    return true;
}

bool Renderer::attach_window(PlatformWindow hwnd, float scale) {
    destroy_target();
    hwnd_ = hwnd;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int px_w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int px_h = std::max(1, static_cast<int>(rc.bottom - rc.top));

    // 这里刻意**不用** ID2D1HwndRenderTarget（硬件加速那条路）。
    //
    // 起因是一次真实闪退：Windows 事件日志里 penhu-native.exe 的「出错模块」
    // 是 igc64.dll —— 那是 Intel 核显驱动，偏移落在驱动的图形代码里。
    // 症状很有迷惑性：停在登录页（图元少）一直没事，登录之后进记账页
    // （52 个分类格 + 卡片 + 列表）就闪退，而且崩在驱动内部，
    // 从我们自己的代码里完全看不出问题。
    //
    // 换成「软件光栅器渲染到 WIC 位图 + GDI 贴到窗口」之后完全不碰显卡驱动。
    // 代价是每帧多一次内存拷贝 —— 这个界面是按需重绘、图元数量在几百级，
    // 实测没有可感知的差异。附带好处：save_png() 对窗口模式也能用了。
    if (!create_software_target(px_w, px_h, scale)) return false;
    view_w_ = cap_w_;
    view_h_ = cap_h_;
    logical_ = {static_cast<float>(view_w_) / scale_, static_cast<float>(view_h_) / scale_};
    return true;
}

bool Renderer::attach_offscreen(int width_px, int height_px, float scale) {
    destroy_target();
    hwnd_ = nullptr;
    if (!create_software_target(width_px, height_px, scale)) return false;
    view_w_ = cap_w_;
    view_h_ = cap_h_;
    logical_ = {static_cast<float>(view_w_) / scale_, static_cast<float>(view_h_) / scale_};
    return true;
}

void Renderer::detach() {
    destroy_target();
    hwnd_ = nullptr;
}

void Renderer::present() {
    if (hwnd_ == nullptr || wic_bitmap_ == nullptr) return;

    const int vw = (view_w_ > 0) ? view_w_ : cap_w_;
    const int vh = (view_h_ > 0) ? view_h_ : cap_h_;
    if (vw <= 0 || vh <= 0) return;
    if (vw > cap_w_ || vh > cap_h_) return;

    WICRect rect{0, 0, static_cast<INT>(vw), static_cast<INT>(vh)};
    ComPtr<IWICBitmapLock> lock;
    // IWICBitmap::Lock 的参数顺序是 (矩形, 标志, 输出)，别按直觉写成 (标志, 矩形, …)
    if (FAILED(wic_bitmap_->Lock(&rect, WICBitmapLockRead, lock.GetAddressOf()))) return;

    UINT stride = 0, buffer_size = 0;
    BYTE* pixels = nullptr;
    lock->GetStride(&stride);
    if (FAILED(lock->GetDataPointer(&buffer_size, &pixels)) || pixels == nullptr) return;

    HDC hdc = GetDC(hwnd_);
    if (hdc == nullptr) return;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    // 宽度必须是**位图真实宽度**（= 容量）：GDI 靠它算行距，
    // 写成视口宽度的话 stride 对不上，画面会斜着错开。
    info.bmiHeader.biWidth = static_cast<LONG>(cap_w_);
    // 负高度 = 自上而下的行序，和 D2D 的内存布局一致。写成正数画面会上下颠倒。
    // 这里只声明视口那么多行，GDI 就只会读前 vh 行。
    info.bmiHeader.biHeight = -static_cast<LONG>(vh);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    SetDIBitsToDevice(hdc, 0, 0, vw, vh, 0, 0, 0, vh, pixels, &info, DIB_RGB_COLORS);
    ReleaseDC(hwnd_, hdc);
}

bool Renderer::resize(int width_px, int height_px, bool allow_reclaim) {
    if (hwnd_ == nullptr) return false;
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);

    // 先改视口：拖拽缩放时这是唯一**必须**做的事，成本接近零。
    view_w_ = w;
    view_h_ = h;
    logical_ = {static_cast<float>(w) / scale_, static_cast<float>(h) / scale_};

    // 容量还装得下就直接用 —— 位图和 D2D 渲染目标都不重建。
    // 这是修「拖拽缩放闪白」的核心：原来这一步每次都要重分配整张位图。
    if (rt_ != nullptr && w <= cap_w_ && h <= cap_h_) {
        // 尺寸连续变化进行中（窗口动画 / 拖拽）只增不减：
        // 回收重建落在运动的某一帧上就是一次可见的卡顿。
        if (!allow_reclaim) return true;
        // 缩得太厉害才回收，避免来回拖的时候反复重建。
        // 判据留了一半的滞回区间，正好卡在「明显变小了」而不是「抖了一下」。
        if (w >= cap_w_ / 2 && h >= cap_h_ / 2) return true;
    }

    // 需要扩容（或回收）：按目标尺寸带余量分配。
    // 余量是宽度的 1/4（下限 384、上限 1024），不是原来的「min(256, w/8)」：
    // 窗口动画每帧步进 70~80px，256 以内的余量两三步就用完 —— 动画全程
    // 每 1~2 帧 destroy+create 一次渲染目标，掉帧与抖动就是这么来的。
    // 拖拽缩放同理：1/4 的余量让连续放大十几步都不用重建。
        const int nw = w + std::clamp(w / 4, 384, 1024);
        const int nh = h + std::clamp(h / 4, 384, 1024);
    destroy_target();   // 不动 hwnd_，所以不用先存后恢复
    return create_software_target(nw, nh, scale_);
}

bool Renderer::reserve(int width_px, int height_px) {
    const int w = std::max(1, width_px);
    const int h = std::max(1, height_px);
    // 装得下就什么都不做 —— 缩小方向的动画（还原 / 退出全屏）常态如此。
    if (rt_ != nullptr && w <= cap_w_ && h <= cap_h_) return true;
    // 不够才重建，**不**再带余量：调用方给的已经是「全程最大尺寸」
    // （窗口动画起终点逐维取大），再留余量只是浪费。
    // 不动 view/logical —— 视口仍按窗口当前尺寸走。
    destroy_target();
    return create_software_target(w, h, scale_);
}

bool Renderer::begin_frame(const Color& clear) {
    if (rt_ == nullptr) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(to_d2d(clear));
    return true;
}

bool Renderer::end_frame() {
    if (rt_ == nullptr) return false;
    const HRESULT hr = rt_->EndDraw();
    return hr != D2DERR_RECREATE_TARGET;
}

D2D1_COLOR_F Renderer::to_d2d(const Color& c) const {
    return D2D1::ColorF(static_cast<float>(c.r()) / 255.0f,
                        static_cast<float>(c.g()) / 255.0f,
                        static_cast<float>(c.b()) / 255.0f,
                        static_cast<float>(c.a()) / 255.0f);
}

void Renderer::fill(const Rect& r, const Color& c) {
    if (rt_ == nullptr || r.empty() || c.a() == 0) return;
    brush_->SetColor(to_d2d(c));
    rt_->FillRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush_.Get());
}

void Renderer::fill_round(const Rect& r, float radius, const Color& c) {
    if (rt_ == nullptr || r.empty() || c.a() == 0) return;
    const float rad = std::max(0.0f, std::min(radius, std::min(r.w, r.h) * 0.5f));
    brush_->SetColor(to_d2d(c));
    if (rad <= 0.01f) {
        rt_->FillRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush_.Get());
        return;
    }
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), rad, rad),
                              brush_.Get());
}

void Renderer::stroke_round(const Rect& r, float radius, const Color& c, float width) {
    if (rt_ == nullptr || r.empty() || c.a() == 0) return;
    const float rad = std::max(0.0f, std::min(radius, std::min(r.w, r.h) * 0.5f));
    brush_->SetColor(to_d2d(c));
    // 描边以路径为中心，半宽会溢出到矩形外。这里按半宽内缩，
    // 让「边框刚好落在给定的矩形里」——否则相邻卡片的描边会互相压住。
    const float half = width * 0.5f;
    const D2D1_RECT_F rr = D2D1::RectF(r.x + half, r.y + half, r.right() - half, r.bottom() - half);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), brush_.Get(), width);
}

void Renderer::fill_circle(float cx, float cy, float radius, const Color& c) {
    if (rt_ == nullptr || radius <= 0.0f || c.a() == 0) return;
    brush_->SetColor(to_d2d(c));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.Get());
}

void Renderer::draw_line(const Point& a, const Point& b, const Color& c, float width) {
    if (rt_ == nullptr || c.a() == 0) return;
    brush_->SetColor(to_d2d(c));
    rt_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), brush_.Get(), width);
}

void Renderer::push_clip(const Rect& r) {
    if (rt_ == nullptr) return;
    rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.right(), r.bottom()),
                             D2D1_ANTIALIAS_MODE_ALIASED);
    ++clip_depth_;
}

void Renderer::pop_clip() {
    if (rt_ == nullptr || clip_depth_ <= 0) return;
    rt_->PopAxisAlignedClip();
    --clip_depth_;
}

IDWriteTextFormat* Renderer::format_for(TypeStyle style, TextAlign ha, bool wrap) const {
    const int key = static_cast<int>(style) * 6 + static_cast<int>(ha) * 2 + (wrap ? 1 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();

    const TypeToken tok = Theme::light().type(style);
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(font_family_.c_str(), nullptr, to_weight(tok.weight),
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         tok.size, L"zh-cn", fmt.GetAddressOf()))) {
        return nullptr;
    }
    switch (ha) {
        case TextAlign::Center: fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); break;
        case TextAlign::Right:  fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); break;
        default:                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); break;
    }
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    // 行距用设计令牌里的确定值，不用字体默认值：默认值随字体变化，
    // 会让「两行备注」这类地方的高度算不准，进而把卡片高度算错。
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, tok.line_height, tok.line_height * 0.78f);

    IDWriteTextFormat* raw = fmt.Get();
    formats_[key] = std::move(fmt);
    return raw;
}

Size Renderer::measure(const std::wstring& text, TypeStyle style, float max_width, bool wrap) const {
    if (dwrite_ == nullptr || text.empty()) return {0.0f, 0.0f};
    IDWriteTextFormat* fmt = format_for(style, TextAlign::Left, wrap);
    if (fmt == nullptr) return {0.0f, 0.0f};

    const float limit = wrap ? std::max(1.0f, max_width) : 100000.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                         limit, 100000.0f, layout.GetAddressOf()))) {
        return {0.0f, 0.0f};
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return {0.0f, 0.0f};
    return {m.widthIncludingTrailingWhitespace, m.height};
}

void Renderer::text(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
                    TextAlign ha, VAlign va, bool wrap, bool ellipsis) {
    if (rt_ == nullptr || s.empty() || box.empty()) return;
    IDWriteTextFormat* fmt = format_for(style, ha, wrap);
    if (fmt == nullptr) return;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt,
                                         std::max(1.0f, box.w), std::max(1.0f, box.h),
                                         layout.GetAddressOf()))) {
        return;
    }
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(layout.Get(), sign.GetAddressOf()))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, sign.Get());
        }
    }

    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);

    float y = box.y;
    if (va == VAlign::Middle) y = box.y + (box.h - m.height) * 0.5f;
    else if (va == VAlign::Bottom) y = box.bottom() - m.height;

    brush_->SetColor(to_d2d(c));
    rt_->DrawTextLayout(D2D1::Point2F(box.x, y), layout.Get(), brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::text_line(const std::wstring& s, const Rect& box, TypeStyle style, const Color& c,
                         TextAlign ha, bool ellipsis) {
    text(s, box, style, c, ha, VAlign::Middle, false, ellipsis);
}

bool Renderer::save_png(const std::string& utf8_path) {
    if (wic_ == nullptr || wic_bitmap_ == nullptr) return false;

    // 只导出**视口**那块。位图是按容量（带余量）分配的，
    // 整张导出的话右边和下边会多出一段从没画过的空白。
    const UINT w = static_cast<UINT>((view_w_ > 0) ? view_w_ : cap_w_);
    const UINT h = static_cast<UINT>((view_h_ > 0) ? view_h_ : cap_h_);
    return encode_png(wic_bitmap_.Get(), w, h, to_wide(utf8_path));
}

/// PNG 编码公共部分：save_png（客户区离屏图）和 save_screen_png（抓屏）共用。
bool Renderer::encode_png(IWICBitmapSource* src, UINT w, UINT h, const std::wstring& path) {
    if (wic_ == nullptr || src == nullptr) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic_->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), bag.GetAddressOf()))) return false;
    if (FAILED(frame->Initialize(bag.Get()))) return false;

    if (FAILED(frame->SetSize(w, h))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    WICRect src_rect{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    if (FAILED(frame->WriteSource(src, &src_rect))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

bool Renderer::load_image(const std::wstring& path, ComPtr<IWICBitmapSource>& out) const {
    if (wic_ == nullptr) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand,
                                               decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.GetAddressOf()))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return false;
    }
    out = converter;
    return true;
}

bool Renderer::load_image_mem(const void* data, size_t bytes, ComPtr<IWICBitmapSource>& out) const {
    if (wic_ == nullptr || data == nullptr || bytes == 0) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic_->CreateStream(stream.GetAddressOf()))) return false;
    // WICInProcPointer 就是非 const 的 BYTE*，接口没有 const 版本
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(static_cast<const BYTE*>(data)),
            static_cast<DWORD>(bytes)))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr,
                                             WICDecodeMetadataCacheOnDemand,
                                             decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.GetAddressOf()))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return false;
    }
    out = converter;
    return true;
}

bool Renderer::save_screen_png(const std::string& utf8_path) {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (wic_ == nullptr || sw <= 0 || sh <= 0) return false;

    HDC scr = GetDC(nullptr);
    if (scr == nullptr) return false;
    HDC mem = CreateCompatibleDC(scr);

    // top-down（负高度）的 32bpp DIB：内存行序从上到下，和 WIC/的使用习惯一致。
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

    bool ok = false;
    if (bmp != nullptr && bits != nullptr) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, sw, sh, scr, 0, 0, SRCCOPY);
        SelectObject(mem, old);

        ComPtr<IWICBitmap> shot;
        if (SUCCEEDED(wic_->CreateBitmapFromMemory(
                static_cast<UINT>(sw), static_cast<UINT>(sh),
                GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(sw * 4),
                static_cast<UINT>(sw * sh * 4), static_cast<BYTE*>(bits),
                shot.GetAddressOf()))) {
            ok = encode_png(shot.Get(), static_cast<UINT>(sw), static_cast<UINT>(sh),
                            to_wide(utf8_path));
        }
    }
    if (bmp != nullptr) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ok;
}

void Renderer::draw_image(IWICBitmapSource* src, const Rect& box, float radius, bool cover) {
    if (rt_ == nullptr || src == nullptr || box.empty()) return;

    UINT iw = 0, ih = 0;
    if (FAILED(src->GetSize(&iw, &ih)) || iw == 0 || ih == 0) return;

    ComPtr<ID2D1Bitmap> bitmap;
    // 每次重建而不缓存：缓存的 key 只能是源指针，而源指针在图片被替换后
    // 可能被复用，会静默画错图。界面是按需重绘，这里重建的开销可以接受。
    if (FAILED(rt_->CreateBitmapFromWicBitmap(src, nullptr, bitmap.GetAddressOf()))) return;

    const float sw = static_cast<float>(iw);
    const float sh = static_cast<float>(ih);

    Rect dst = box;
    if (cover) {
        const float scale = std::max(box.w / sw, box.h / sh);
        const float dw = sw * scale;
        const float dh = sh * scale;
        dst = {box.x + (box.w - dw) * 0.5f, box.y + (box.h - dh) * 0.5f, dw, dh};
    } else {
        const float scale = std::min(box.w / sw, box.h / sh);
        const float dw = sw * scale;
        const float dh = sh * scale;
        dst = {box.x + (box.w - dw) * 0.5f, box.y + (box.h - dh) * 0.5f, dw, dh};
    }

    if (radius > 0.01f) push_clip(box);
    rt_->DrawBitmap(bitmap.Get(), D2D1::RectF(dst.x, dst.y, dst.right(), dst.bottom()),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    if (radius > 0.01f) pop_clip();
}

}  // namespace penhu::native::ui
