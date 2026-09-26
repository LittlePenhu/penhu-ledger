// =============================================================================
//  native/app/image_util.cpp
// =============================================================================

#include "app/image_util.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace penhu::native {

using Microsoft::WRL::ComPtr;

namespace {

/// 当前线程的 COM 套间守卫。
///
/// 为什么必须有它：**COM 的套间（apartment）是按线程算的**。
/// `main.cpp` 只在主线程 `CoInitializeEx(STA)` 过一次；而「扫描识别」
/// 跑在 `app.cpp` 的 `std::thread` 里，那条线程从没初始化 COM ——
/// 于是 `CoCreateInstance(CLSID_WICImagingFactory)` 直接返回
/// `CO_E_NOTINITIALIZED (0x800401F0)`，工厂为空，
/// 上层把它翻译成「图片子系统初始化失败」。用户看到的就是这句话。
///
/// 实测（同一台机器同一个进程，唯一变量是「线程内有没有初始化」）：
///   主线程 STA          → S_OK，拿到工厂
///   工作线程 未初始化   → 0x800401F0，拿不到
///   工作线程 MTA        → S_OK，拿到
///
/// 放在这一层而不是各个调用方，是因为「要碰 WIC（= COM）就得先有套间」
/// 是 image_util 自己的约束。以后再有工作线程来调它，不该重踩一次。
struct ComApartment {
    /// true 表示这次是我们初始化的，析构时要收尾；
    /// 已经初始化过（S_FALSE / RPC_E_CHANGED_MODE）就不该我们管。
    bool owned{false};

    ComApartment() noexcept {
        // 用 MTA：WIC 的工厂是 Both 线程模型，MTA 下可用。
        // 主线程已经是 STA，这里会返回 RPC_E_CHANGED_MODE ——
        // 同一个进程里两种套间对 WIC 都成立，继续往下用即可。
        owned = (CoInitializeEx(nullptr, COINIT_MULTITHREADED) == S_OK);
    }
    ~ComApartment() {
        if (owned) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(kB64[(v >> 6) & 0x3F]);
        out.push_back(kB64[v & 0x3F]);
    }
    if (i < len) {
        const size_t rem = len - i;
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (rem == 2) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(rem == 2 ? kB64[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

ComPtr<IWICImagingFactory> make_wic() {
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(wic.GetAddressOf()));
    return wic;
}

ComPtr<IWICBitmapSource> decode(IWICImagingFactory* wic, const std::wstring& path,
                                UINT& w, UINT& h, WICPixelFormatGUID target_format,
                                std::wstring* err) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf()))) {
        if (err) *err = L"打不开这个文件（可能不是图片，或者没有读取权限）";
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        if (err) *err = L"这个文件里没有可用的图像帧";
        return nullptr;
    }
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
        if (err) *err = L"读不出图片尺寸（文件可能已损坏）";
        return nullptr;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf())) ||
        FAILED(conv->Initialize(frame.Get(), target_format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut))) {
        if (err) *err = L"这个图片的像素格式不支持（换 PNG 或 JPG 再试）";
        return nullptr;
    }
    return conv;
}

}  // namespace

bool probe_image_size(const std::wstring& path, int& width, int& height) {
    const ComApartment com;   // 见 ComApartment 的注释：工作线程里没它必然失败
    auto wic = make_wic();
    if (wic == nullptr) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h))) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

bool make_image_data_url(const std::wstring& path, int max_edge, std::string& out_url,
                         std::wstring* err) {
    // 这一句是「图片子系统初始化失败」的解药：识别跑在工作线程上，
    // 那条线程没有 COM 套间，不初始化的话下面 CoCreateInstance 必失败。
    const ComApartment com;
    auto wic = make_wic();
    if (wic == nullptr) {
        if (err) *err = L"图片子系统初始化失败";
        return false;
    }

    UINT w = 0, h = 0;
    // JPEG 编码器要 24bppBGR，所以直接按它解码，省一次转换
    ComPtr<IWICBitmapSource> src = decode(wic.Get(), path, w, h, GUID_WICPixelFormat24bppBGR, err);
    if (src == nullptr) return false;

    UINT out_w = w;
    UINT out_h = h;
    if (max_edge > 0 && static_cast<int>(std::max(w, h)) > max_edge) {
        const float scale = static_cast<float>(max_edge) / static_cast<float>(std::max(w, h));
        out_w = std::max<UINT>(1, static_cast<UINT>(static_cast<float>(w) * scale));
        out_h = std::max<UINT>(1, static_cast<UINT>(static_cast<float>(h) * scale));

        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
            FAILED(scaler->Initialize(src.Get(), out_w, out_h, WICBitmapInterpolationModeFant))) {
            if (err) *err = L"图片缩放失败";
            return false;
        }
        ComPtr<IWICBitmapSource> scaled = scaler;
        src = scaled;
    }

    // 编码到内存流
    ComPtr<IStream> mem;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, mem.GetAddressOf()))) {
        if (err) *err = L"内存流创建失败";
        return false;
    }

    ComPtr<IWICStream> wstream;
    if (FAILED(wic->CreateStream(wstream.GetAddressOf())) ||
        FAILED(wstream->InitializeFromIStream(mem.Get()))) {
        if (err) *err = L"图片流初始化失败";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(wstream.Get(), WICBitmapEncoderNoCache))) {
        if (err) *err = L"JPEG 编码器初始化失败";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf()))) {
        if (err) *err = L"JPEG 帧创建失败";
        return false;
    }
    if (props != nullptr) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.85f;   // 0.85 是「看得清文字」和「体积可接受」之间的常用折中
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get()))) {
        if (err) *err = L"JPEG 帧初始化失败";
        return false;
    }

    frame->SetSize(out_w, out_h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&pf);
    if (FAILED(frame->WriteSource(src.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        if (err) *err = L"JPEG 编码失败";
        return false;
    }

    STATSTG stat{};
    if (FAILED(mem->Stat(&stat, STATFLAG_NONAME))) {
        if (err) *err = L"读取编码结果失败";
        return false;
    }
    const size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
    if (size == 0) {
        if (err) *err = L"编码结果为空";
        return false;
    }

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(mem.Get(), &hg)) || hg == nullptr) {
        if (err) *err = L"取编码缓冲失败";
        return false;
    }
    const void* data = GlobalLock(hg);
    if (data == nullptr) {
        if (err) *err = L"锁定编码缓冲失败";
        return false;
    }

    out_url = "data:image/jpeg;base64,";
    out_url += base64_encode(static_cast<const unsigned char*>(data), size);
    GlobalUnlock(hg);
    return true;
}

}  // namespace penhu::native
