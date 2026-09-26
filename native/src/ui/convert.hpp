#pragma once
// =============================================================================
//  native/ui/convert.hpp
//  UTF-8 <-> UTF-16(宽字符) 转换。
//
//  为什么需要它：core 全用 UTF-8 std::string（数据库、JSON、日志都是），
//  而界面层用 std::wstring（Windows 的 Win32 / DirectWrite 要 UTF-16，
//  Linux 侧 Pango 要 UTF-8，所以宽字符成了两边的「公共中间态」）。
//  这条边界在 UI 层到处都是，集中放这里。
//
//  注意：**不要**用「把每个 char 直接 static_cast<wchar_t>」那种写法，
//  它对 ASCII 之外的所有字符都是错的（中文会变成乱码）。这个坑在日志里出现过。
//
//  2026-09-22 移植 Linux 时改过：原来这个文件 include <windows.h> 靠
//  MultiByteToWideChar 实现，于是**任何**引用它的共享代码都被绑死在 Windows 上。
//  现在是自己实现的纯 C++ 版本，两个平台共用同一份 —— 少一处「只在某个平台编不过」。
// =============================================================================

#include <cstdint>
#include <string>

namespace penhu::native::ui {

inline std::wstring to_wide(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F; extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F; extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07; extra = 3;
        } else {
            // 非法起始字节：跳过而不是报错。这种数据只可能来自被损坏的文件，
            // 让它渲染成一个替换字符，比让整个界面转换失败合理。
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= n) {
            out.push_back(0xFFFD);   // 截断的序列
            break;
        }
        bool bad = false;
        for (size_t k = 1; k <= extra; ++k) {
            const uint8_t bk = static_cast<uint8_t>(utf8[i + k]);
            if ((bk & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (bad) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += extra + 1;

        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            // 一律转成 UTF-16 代理对。wchar_t 在 Windows 是 16 位、在 Linux 是 32 位，
            // 但两边都用代理对表示，同一份代码在两平台产生同样的 wstring ——
            // 这样比较字符串、算长度、切子串的行为就不会有平台差异。
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

inline std::string to_utf8(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size() * 3);
    for (size_t i = 0; i < wide.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(wide[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size()) {
            const uint32_t lo = static_cast<uint32_t>(wide[i + 1]);
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

}  // namespace penhu::native::ui
