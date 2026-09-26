// =============================================================================
//  native/pages/page_common.cpp
// =============================================================================

#include "pages/page_common.hpp"

#include <algorithm>

#include "ui/renderer.hpp"

namespace penhu::native {

using namespace ui;

// -----------------------------------------------------------------------------
//  info_line
// -----------------------------------------------------------------------------

Label* info_line(VBox& col, const Theme& th, const std::wstring& text) {
    if (text.empty()) return nullptr;
    auto* lb = col.emplace<Label>();
    lb->text = text;
    // BodySmall（12px）+ 次要色：它是说明，不该和正文抢注意力
    lb->style = TypeStyle::BodySmall;
    lb->color = th.palette.on_surface_variant;
    lb->wrap = false;       // 一行就够，长了省略 ——
    lb->ellipsis = true;    // 换行会把下面的内容整体往下推，不划算
    return lb;
}

//  ErrorBanner
// -----------------------------------------------------------------------------

float ErrorBanner::preferred_height(float width, Renderer& r) {
    if (text.empty()) return 0.0f;
    const float inner = std::max(60.0f, width - 30.0f);
    // 一行 16 的行高 + 上下各 10 的留白，最少 40
    return std::max(40.0f, r.measure(text, TypeStyle::BodySmall, inner, true).h + 20.0f);
}

void ErrorBanner::paint(Renderer& r, const Theme& th) {
    if (text.empty()) return;
    r.fill_round(bounds_, Theme::kCornerSm, th.palette.error_container);
    r.text(text, {bounds_.x + 14.0f, bounds_.y + 6.0f, bounds_.w - 28.0f, bounds_.h - 12.0f},
           TypeStyle::BodySmall, th.palette.on_error_container, TextAlign::Left, VAlign::Middle,
           true, true);
}

// -----------------------------------------------------------------------------
//  InfoNote
// -----------------------------------------------------------------------------

float InfoNote::preferred_height(float width, Renderer& r) {
    if (text.empty()) return 0.0f;
    const float inner = std::max(60.0f, width - 30.0f);
    return std::max(38.0f, r.measure(text, TypeStyle::BodySmall, inner, true).h + 20.0f);
}

void InfoNote::paint(Renderer& r, const Theme& th) {
    if (text.empty()) return;
    const Color bg = warn ? (th.is_dark ? th.palette.tertiary_container.alpha_of(0.55f)
                                        : th.palette.tertiary_container)
                          : th.palette.surface_container_high;
    const Color fg = warn ? th.palette.on_tertiary_container : th.palette.on_surface_variant;
    r.fill_round(bounds_, Theme::kCornerSm, bg);
    r.text(text, {bounds_.x + 14.0f, bounds_.y + 6.0f, bounds_.w - 28.0f, bounds_.h - 12.0f},
           TypeStyle::BodySmall, fg, TextAlign::Left, VAlign::Middle, true, true);
}

// -----------------------------------------------------------------------------
//  SectionTitle
// -----------------------------------------------------------------------------

void SectionTitle::paint(Renderer& r, const Theme& th) {
    const float tw = trailing.empty() ? bounds_.w : std::max(60.0f, bounds_.w - 220.0f);
    r.text_line(title, {bounds_.x + 4.0f, bounds_.y, tw, bounds_.h}, TypeStyle::TitleSmall,
                th.palette.on_surface, TextAlign::Left, true);
    if (!trailing.empty()) {
        r.text_line(trailing, {bounds_.right() - 240.0f, bounds_.y, 240.0f, bounds_.h},
                    TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Right, true);
    }
}

std::wstring short_path(const std::wstring& path) {
    constexpr size_t kKeep = 2;   // 末尾保留的段数

    std::vector<std::wstring> segs;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            if (i > start) segs.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    // 段数不多就直接原样显示 —— 短的路径加省略号反而更难认
    if (segs.size() <= kKeep + 1) return path;

    std::wstring out = path.substr(0, 2) + L"\\…";   // 例如 "C:\…"
    for (size_t i = segs.size() - kKeep; i < segs.size(); ++i) {
        out += L"\\" + segs[i];
    }
    return out;
}

// -----------------------------------------------------------------------------
//  StatCell
// -----------------------------------------------------------------------------

void StatCell::paint(Renderer& r, const Theme& th) {
    const Color c = color_unset(value_color) ? th.palette.on_surface : value_color;
    r.text_line(value, {bounds_.x + 4.0f, bounds_.y + 4.0f, bounds_.w - 8.0f, 30.0f},
                TypeStyle::TitleLarge, c, TextAlign::Left, true);
    r.text_line(label, {bounds_.x + 4.0f, bounds_.y + 34.0f, bounds_.w - 8.0f, 18.0f},
                TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, true);
}

}  // namespace penhu::native
