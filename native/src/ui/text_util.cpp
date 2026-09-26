// =============================================================================
//  native/ui/text_util.cpp
//  文本净化：两个平台共用的部分。
//
//  原来 sanitize_single_line 写在 platform_input.cpp（Windows 实现）里，
//  但它其实**没有任何平台依赖** —— 它只是把多行文本折成单行。
//  移植时把它挪出来，免得 Linux 侧要么复制一份、要么去链接一个
//  「只有 Windows 才编」的目标。
//
//  判断一段代码该放哪一层，标准是「它依赖平台吗」，不是「它现在在哪个文件里」。
// =============================================================================

#include <string>

#include "ui/platform_input.hpp"

namespace penhu::native::ui {

std::wstring sanitize_single_line(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());

    for (const wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            // 折成空格而不是直接丢掉：两行内容丢掉换行会粘成一串，
            // 折成空格至少语义还在（多行文本粘进单行框本就该是这个结果）。
            if (!out.empty() && out.back() != L' ') out.push_back(L' ');
        } else if (c >= 32) {
            out.push_back(c);
        }
    }

    // 去掉首尾空白。这一步是**必须**的：从网页或邮件复制的 API Key
    // 常常带一个尾部换行，粘进输入框后 key 校验会失败，
    // 而框里看上去「一模一样」—— 最难查的一类故障。
    size_t first = 0;
    while (first < out.size() && out[first] == L' ') ++first;
    size_t last = out.size();
    while (last > first && out[last - 1] == L' ') --last;
    if (first >= last) return {};
    return std::wstring(out.begin() + static_cast<long>(first),
                        out.begin() + static_cast<long>(last));
}

}  // namespace penhu::native::ui
