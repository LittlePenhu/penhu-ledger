// =============================================================================
//  native/app/image_util_linux.cpp
//  本地图片 → 多模态接口可用的 data URL（Linux 实现）。
//
//  Windows 那份靠 WIC（解码 + 缩放 + JPEG 编码一条龙）。Linux 没有等价物，
//  所以用 libpng + libjpeg-turbo 自己走一遍：
//
//    读文件 → 按文件头判格式解码到 RGBA → 等比缩到最长边 ≤ max_edge
//      → 编码 JPEG(q=0.85) → base64 → "data:image/jpeg;base64,..."
//
//  为什么还是编码成 JPEG 而不是直接把 PNG 发出去：
//  手机支付截图常见 3–5 MB（PNG），base64 之后还要涨 1/3，多数多模态接口
//  对单图有 4 MB 左右硬限制 —— 原图直发基本就是 400。
//  缩放 + JPEG 之后同一条截图通常落到 100–300 KB，差了二十倍。
//
//  「绝不静默降级」这条在这里同样适用：任何一步失败都要给出**可读的中文原因**，
//  因为这段文本会直接显示在识别页上。
// =============================================================================

// 顺序有讲究：jpeglib.h 用了 size_t / FILE 却没自己 include，
// 直接放在最前面会报一堆「'size_t' does not name a type」。
#include <cstddef>
#include <cstdio>

#include <jpeglib.h>
#include <png.h>

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <string>
#include <vector>

#include "app/image_util.hpp"
#include "penhu/util/log.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {

/// 一张解好的图：RGBA8，行紧密排列（无 stride padding）。
/// alpha 一律归一到 255（支付截图没有透明需求，省得后面处处判）。
struct Image {
    int                w{0};
    int                h{0};
    std::vector<uint8_t> rgba;   // w * h * 4
};

// ---- libjpeg 的错误接管 ---------------------------------------------------
// 默认错误处理是直接 exit 进程。打开一张损坏的图就把程序干掉，不能接受。

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

void jpeg_output_message_cb(j_common_ptr) { /* 警告不打断解码 */ }

bool decode_png(const uint8_t* data, size_t size, Image& out, std::wstring* err) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    // 用 libpng 的简化接口（png_image），它把「读 header → 设变换 → 逐行读」
    // 都包好了；手写那套 state machine 容易在 iCCP/交错这些边角上出错。
    if (png_image_begin_read_from_memory(&image, data, size) == 0) {
        if (err != nullptr) *err = L"PNG 文件读不了（可能损坏或被截断）";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    out.w = static_cast<int>(image.width);
    out.h = static_cast<int>(image.height);
    out.rgba.resize(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) == 0) {
        if (err != nullptr) *err = L"PNG 解码失败：" + ui::to_wide(image.message);
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    return true;
}

bool decode_jpeg(const uint8_t* data, size_t size, Image& out, std::wstring* err) {
    jpeg_decompress_struct cinfo{};
    JpegError jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_cb;
    jerr.pub.output_message = jpeg_output_message_cb;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        if (err != nullptr) {
            *err = L"JPEG 解码失败：" + ui::to_wide(std::string(jerr.message));
        }
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = TRUE;
    jpeg_start_decompress(&cinfo);

    out.w = static_cast<int>(cinfo.output_width);
    out.h = static_cast<int>(cinfo.output_height);
    if (out.w <= 0 || out.h <= 0) {
        jpeg_destroy_decompress(&cinfo);
        if (err != nullptr) *err = L"JPEG 尺寸不合法";
        return false;
    }
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 255);

    std::vector<uint8_t> row(static_cast<size_t>(out.w) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* rows[1] = {row.data()};
        const JDIMENSION y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * out.w * 4;
        for (int x = 0; x < out.w; ++x) {
            dst[x * 4 + 0] = row[static_cast<size_t>(x) * 3 + 0];
            dst[x * 4 + 1] = row[static_cast<size_t>(x) * 3 + 1];
            dst[x * 4 + 2] = row[static_cast<size_t>(x) * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

/// 等比缩放到最长边 <= max_edge。
/// 用**盒式平均**（把源图上对应的那一小块取平均）而不是最近邻：
/// 支付截图里「金额」是细笔画文字，最近邻在小比例缩放下会把笔画拆断，
/// 表现为识别结果里金额少一位 —— 这类错误用户很难看出来是缩放造成的。
void resize_box(const Image& src, int max_edge, Image& dst) {
    const int longest = std::max(src.w, src.h);
    if (longest <= max_edge) {
        dst = src;
        return;
    }

    const double k = static_cast<double>(max_edge) / static_cast<double>(longest);
    dst.w = std::max(1, static_cast<int>(src.w * k + 0.5));
    dst.h = std::max(1, static_cast<int>(src.h * k + 0.5));
    dst.rgba.assign(static_cast<size_t>(dst.w) * dst.h * 4, 0);

    for (int dy = 0; dy < dst.h; ++dy) {
        const int sy0 = dy * src.h / dst.h;
        int sy1 = (dy + 1) * src.h / dst.h;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst.w; ++dx) {
            const int sx0 = dx * src.w / dst.w;
            int sx1 = (dx + 1) * src.w / dst.w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = sy0; sy < sy1 && sy < src.h; ++sy) {
                const uint8_t* line = src.rgba.data() + static_cast<size_t>(sy) * src.w * 4;
                for (int sx = sx0; sx < sx1 && sx < src.w; ++sx) {
                    acc[0] += line[sx * 4 + 0];
                    acc[1] += line[sx * 4 + 1];
                    acc[2] += line[sx * 4 + 2];
                    acc[3] += line[sx * 4 + 3];
                    ++n;
                }
            }
            if (n == 0) n = 1;
            uint8_t* d = dst.rgba.data() + (static_cast<size_t>(dy) * dst.w + dx) * 4;
            d[0] = static_cast<uint8_t>(acc[0] / n);
            d[1] = static_cast<uint8_t>(acc[1] / n);
            d[2] = static_cast<uint8_t>(acc[2] / n);
            d[3] = static_cast<uint8_t>(acc[3] / n);
        }
    }
}

std::string base64_encode(const uint8_t* data, size_t size) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < size) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    const size_t left = size - i;
    if (left == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (left == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

}  // namespace

bool make_image_data_url(const std::wstring& path, int max_edge, std::string& out_url,
                         std::wstring* err) {
    out_url.clear();
    const std::string utf8 = ui::to_utf8(path);

    std::vector<uint8_t> bytes;
    if (!read_file(utf8, bytes)) {
        if (err != nullptr) *err = L"图片读不出来：文件不存在或没有读取权限";
        return false;
    }

    // 按**文件头**判格式，不看扩展名：用户把 .png 改名成 .jpg 是常事，
    // 按扩展名分会解不了然后报「格式不支持」，而文件其实是好的。
    Image decoded;
    bool ok = false;
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G') {
        ok = decode_png(bytes.data(), bytes.size(), decoded, err);
    } else if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        ok = decode_jpeg(bytes.data(), bytes.size(), decoded, err);
    } else {
        if (err != nullptr) {
            *err = L"这个格式识别不了（只支持 PNG 与 JPEG，BMP/WEBP 暂不支持）";
        }
        return false;
    }
    if (!ok) return false;

    Image scaled;
    resize_box(decoded, max_edge, scaled);

    // JPEG 质量 0.85：支付截图的文字边缘在这个质量下仍然清晰，
    // 而体积比 0.95 小一半左右。识别模型看的是文字，不是照片质感。
    std::vector<uint8_t> jpeg_buf;
    {
        jpeg_compress_struct cinfo{};
        JpegError jerr{};
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpeg_error_exit_cb;
        jerr.pub.output_message = jpeg_output_message_cb;

        if (setjmp(jerr.jump)) {
            jpeg_destroy_compress(&cinfo);
            if (err != nullptr) {
                *err = L"JPEG 编码失败：" + ui::to_wide(std::string(jerr.message));
            }
            return false;
        }

        jpeg_create_compress(&cinfo);
        unsigned char* mem = nullptr;
        unsigned long mem_size = 0;
        jpeg_mem_dest(&cinfo, &mem, &mem_size);

        cinfo.image_width = static_cast<JDIMENSION>(scaled.w);
        cinfo.image_height = static_cast<JDIMENSION>(scaled.h);
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, 85, TRUE);
        jpeg_start_compress(&cinfo, TRUE);

        std::vector<uint8_t> rgb(static_cast<size_t>(scaled.w) * 3);
        while (cinfo.next_scanline < cinfo.image_height) {
            const uint8_t* src_line =
                scaled.rgba.data() + static_cast<size_t>(cinfo.next_scanline) * scaled.w * 4;
            for (int x = 0; x < scaled.w; ++x) {
                rgb[static_cast<size_t>(x) * 3 + 0] = src_line[x * 4 + 0];
                rgb[static_cast<size_t>(x) * 3 + 1] = src_line[x * 4 + 1];
                rgb[static_cast<size_t>(x) * 3 + 2] = src_line[x * 4 + 2];
            }
            JSAMPROW rows[1] = {rgb.data()};
            jpeg_write_scanlines(&cinfo, rows, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_buf.assign(mem, mem + mem_size);
        // jpeg_mem_dest 分配的内存由我们自己释放
        std::free(mem);
        jpeg_destroy_compress(&cinfo);
    }

    out_url = "data:image/jpeg;base64," + base64_encode(jpeg_buf.data(), jpeg_buf.size());
    Logger::default_logger().info(
        "图片已缩放编码: " + std::to_string(decoded.w) + "x" + std::to_string(decoded.h) +
        " → " + std::to_string(scaled.w) + "x" + std::to_string(scaled.h) +
        "，data URL " + std::to_string(out_url.size() / 1024) + " KB");
    return true;
}

bool probe_image_size(const std::wstring& path, int& width, int& height) {
    width = 0;
    height = 0;

    std::vector<uint8_t> bytes;
    if (!read_file(ui::to_utf8(path), bytes)) return false;

    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G') {
        // PNG 的 IHDR 紧跟在 8 字节签名 + 4 字节长度 + "IHDR" 之后
        if (bytes.size() < 24) return false;
        width = static_cast<int>((static_cast<uint32_t>(bytes[16]) << 24) |
                                 (static_cast<uint32_t>(bytes[17]) << 16) |
                                 (static_cast<uint32_t>(bytes[18]) << 8) |
                                 static_cast<uint32_t>(bytes[19]));
        height = static_cast<int>((static_cast<uint32_t>(bytes[20]) << 24) |
                                  (static_cast<uint32_t>(bytes[21]) << 16) |
                                  (static_cast<uint32_t>(bytes[22]) << 8) |
                                  static_cast<uint32_t>(bytes[23]));
        return width > 0 && height > 0;
    }

    if (bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        // JPEG 要扫段找 SOFn，别自己解析得太聪明 —— 交给 libjpeg。
        jpeg_decompress_struct cinfo{};
        JpegError jerr{};
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpeg_error_exit_cb;
        jerr.pub.output_message = jpeg_output_message_cb;
        if (setjmp(jerr.jump)) {
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
        width = static_cast<int>(cinfo.image_width);
        height = static_cast<int>(cinfo.image_height);
        jpeg_destroy_decompress(&cinfo);
        return width > 0 && height > 0;
    }

    return false;
}

}  // namespace penhu::native
