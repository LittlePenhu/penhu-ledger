// =============================================================================
//  native/ui/controls.cpp
//  组件绘制。排版尺寸全部来自设计令牌，不出现"随手"的数值。
// =============================================================================

#include "ui/controls.hpp"

#include <algorithm>
#include <cmath>

#include "ui/keys.hpp"
#include "ui/platform_input.hpp"
#include "ui/renderer.hpp"

namespace penhu::native::ui {

namespace {
constexpr float kPadH = 16.0f;   // 卡片 / 行的水平内边距
constexpr float kPadV = 16.0f;   // 卡片的垂直内边距
/// 单个输入框的字符上限。够放下一整条 API Key / 一长句备注，
/// 又能挡住「误把整个文件粘进来」这种事故。
constexpr size_t kMaxTextLength = 512;
}  // namespace

// -----------------------------------------------------------------------------
//  Label
// -----------------------------------------------------------------------------

float Label::preferred_height(float width, Renderer& r) {
    if (text.empty()) return 0.0f;
    const Size m = r.measure(text, style, width, wrap);
    // 至少给一行的高度：空文本或测量失败时也不塌成 0，
    // 否则卡片高度会随着「有没有内容」跳变。
    const float one_line = Theme::light().type(style).line_height;
    return std::max(m.h, one_line);
}

void Label::paint(Renderer& r, const Theme& th) {
    if (text.empty()) return;
    const Color c = color_unset(color) ? th.palette.on_surface : color;
    r.text(text, bounds_, style, c, align, valign, wrap, ellipsis);
}

// -----------------------------------------------------------------------------
//  ListRow
// -----------------------------------------------------------------------------

float ListRow::preferred_height(float, Renderer&) {
    return subtitle.empty() ? 56.0f : 68.0f;
}

void ListRow::layout(const Rect& area, Renderer&) {
    bounds_ = area;
}

void ListRow::paint(Renderer& r, const Theme& th) {
    const bool has_dot = !color_unset(dot);
    float text_x = bounds_.x + kPadH;

    if (hover_) r.fill_round(bounds_, Theme::kCornerSm,
                             th.palette.on_surface.alpha_of(Theme::kStateHover));

    if (has_dot) {
        r.fill_circle(bounds_.x + kPadH + 10.0f, bounds_.center_y(), 10.0f, dot);
        text_x = bounds_.x + kPadH + 32.0f;
    } else if (!leading_glyph.empty()) {
        const Rect box{bounds_.x + 10.0f, bounds_.center_y() - 16.0f, 32.0f, 32.0f};
        r.fill_round(box, Theme::kCornerFull, th.palette.surface_container_highest);
        r.text_line(leading_glyph, box, TypeStyle::BodyMedium, th.palette.on_surface_variant,
                    TextAlign::Center);
        text_x = bounds_.x + kPadH + 34.0f;
    }

    const float trailing_w = trailing.empty() ? 0.0f : 104.0f;
    const float text_w = std::max(20.0f, bounds_.right() - kPadH - trailing_w - text_x);

    if (subtitle.empty()) {
        r.text_line(title, {text_x, bounds_.y, text_w, bounds_.h}, TypeStyle::BodyLarge,
                    th.palette.on_surface, TextAlign::Left, true);
    } else {
        r.text_line(title, {text_x, bounds_.y + 12.0f, text_w, 22.0f}, TypeStyle::BodyLarge,
                    th.palette.on_surface, TextAlign::Left, true);
        r.text_line(subtitle, {text_x, bounds_.y + 34.0f, text_w, 20.0f}, TypeStyle::BodySmall,
                    th.palette.on_surface_variant, TextAlign::Left, true);
    }

    if (!trailing.empty()) {
        const Color c = color_unset(trailing_color) ? th.palette.on_surface : trailing_color;
        r.text_line(trailing, {bounds_.right() - kPadH - trailing_w, bounds_.y, trailing_w, bounds_.h},
                    TypeStyle::TitleMedium, c, TextAlign::Right, true);
    }

    // 分隔线：M3 的列表用 1px outline-variant，内缩到文字起始位置
    const Color line = th.palette.outline_variant.alpha_of(0.6f);
    r.fill({text_x, bounds_.bottom() - 1.0f, std::max(0.0f, bounds_.right() - text_x), 1.0f}, line);
}

bool ListRow::on_pointer_move(const Point&, bool inside) {
    hover_ = inside;
    return true;
}

bool ListRow::on_pointer_down(const Point&) {
    pressed_ = true;
    return true;
}

bool ListRow::on_pointer_up(const Point&) {
    if (!pressed_) return false;
    pressed_ = false;
    if (on_click) on_click();
    return true;
}

// -----------------------------------------------------------------------------
//  Card
// -----------------------------------------------------------------------------

void Card::paint(Renderer& r, const Theme& th) {
    const Color bgc = color_unset(bg) ? th.palette.surface_container_low : bg;
    r.fill_round(bounds_, radius, bgc);
    if (outlined) {
        r.stroke_round(bounds_, radius, th.palette.outline_variant, 1.0f);
    }
    VBox::paint(r, th);
}

// -----------------------------------------------------------------------------
//  Button
// -----------------------------------------------------------------------------

float Button::preferred_height(float width, Renderer& r) {
    const float h = std::max(min_height, 40.0f);
    const float text_w = std::max(20.0f, width - 32.0f);
    const Size m = r.measure(label, style, text_w, false);
    // 文字比按钮宽时不再加高，交给 ellipsis；这里只保证单行放得下
    (void)m;
    return h;
}

void Button::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty()) return;

    const float radius = Theme::kCornerFull;
    const bool dis = !enabled;

    Color fg = Color::from_rgb(0x000000);
    Color bg = Color::from_rgb(0x000000);

    switch (variant) {
        case Variant::Filled:
            bg = th.palette.primary;
            fg = th.palette.on_primary;
            break;
        case Variant::Tonal:
            bg = th.palette.secondary_container;
            fg = th.palette.on_secondary_container;
            break;
        case Variant::Outlined:
            fg = th.palette.primary;
            break;
        case Variant::Text:
            fg = th.palette.primary;
            break;
        case Variant::Danger:
            bg = th.palette.error;
            fg = th.palette.on_error;
            break;
    }

    if (dis) {
        // M3 的禁用态是「容器和文字一起降级」，而且**不能先画主色再叠一层半透明**：
        // 主色是深紫，上面叠 12% 的黑仍然是深紫，于是 38% 的灰字照样读不出来。
        // 正确做法是不画主色容器，直接画 12% 的 on_surface —— 它会和页面底色混合，
        // 得到的是一块浅灰，字才看得清。（这版是被截图打回来重做的。）
        if (variant == Variant::Outlined) {
            r.stroke_round(bounds_, radius, th.palette.on_surface.alpha_of(0.12f), 1.0f);
        } else if (variant != Variant::Text) {
            r.fill_round(bounds_, radius, th.palette.on_surface.alpha_of(0.12f));
        }
        fg = th.palette.on_surface.alpha_of(0.38f);
    } else {
        if (variant != Variant::Outlined && variant != Variant::Text) {
            r.fill_round(bounds_, radius, bg);
        } else if (variant == Variant::Outlined) {
            r.stroke_round(bounds_, radius, th.palette.outline, 1.0f);
        }

        // 状态层：hover 8% / pressed 10%，叠在按钮本身上
        const float st = pressed_ ? Theme::kStatePressed : (hover_ ? Theme::kStateHover : 0.0f);
        if (st > 0.0f) {
            r.fill_round(bounds_, radius, fg.alpha_of(st));
        }
    }

    std::wstring shown = label;
    if (!glyph.empty()) shown = glyph + L"  " + label;

    r.text_line(shown, bounds_, style, fg, TextAlign::Center, true);
}

bool Button::on_pointer_move(const Point&, bool inside) {
    hover_ = inside;
    return true;
}

bool Button::on_pointer_down(const Point&) {
    if (!enabled) return false;
    pressed_ = true;
    return true;
}

bool Button::on_pointer_up(const Point&) {
    if (!enabled) return false;
    const bool fire = pressed_;
    pressed_ = false;
    if (fire && on_click) on_click();
    return true;
}

bool Button::on_key(unsigned vk, bool down) {
    if (!enabled || !down) return false;
    if (vk == key::Enter || vk == key::Space) {
        if (on_click) on_click();
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Chip
// -----------------------------------------------------------------------------

void Chip::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty()) return;

    const Color bg = selected ? th.palette.secondary_container : th.palette.surface_container;
    const Color fg = selected ? th.palette.on_secondary_container : th.palette.on_surface_variant;
    const Color dotc = color_unset(dot) ? th.palette.outline : dot;

    r.fill_round(bounds_, Theme::kCornerMd, bg);

    const float st = pressed_ ? Theme::kStatePressed : (hover_ ? Theme::kStateHover : 0.0f);
    if (st > 0.0f) r.fill_round(bounds_, Theme::kCornerMd, fg.alpha_of(st));

    if (selected) r.stroke_round(bounds_, Theme::kCornerMd, th.palette.secondary, 1.5f);

    // 色点 + 文字：垂直方向按「点 24 + 间距 6 + 文字 20」排，整体居中
    const float content_h = 24.0f + 6.0f + 20.0f;
    const float top = bounds_.y + (bounds_.h - content_h) * 0.5f;
    r.fill_circle(bounds_.center_x(), top + 12.0f, 10.0f, dotc);
    r.text_line(text, {bounds_.x + 4.0f, top + 30.0f, bounds_.w - 8.0f, 20.0f},
                TypeStyle::BodySmall, selected ? th.palette.on_secondary_container : th.palette.on_surface,
                TextAlign::Center, true);
}

bool Chip::on_pointer_move(const Point&, bool inside) {
    hover_ = inside;
    return true;
}

bool Chip::on_pointer_down(const Point&) {
    pressed_ = true;
    return true;
}

bool Chip::on_pointer_up(const Point&) {
    if (!pressed_) return false;
    pressed_ = false;
    if (on_click) on_click();
    return true;
}

// -----------------------------------------------------------------------------
//  SegmentedTabs
// -----------------------------------------------------------------------------

void SegmentedTabs::paint(Renderer& r, const Theme& th) {
    if (items.empty() || bounds_.empty()) return;

    r.fill_round(bounds_, Theme::kCornerFull, th.palette.surface_container);
    const float seg_w = bounds_.w / static_cast<float>(items.size());

    for (size_t i = 0; i < items.size(); ++i) {
        const Rect seg{bounds_.x + seg_w * static_cast<float>(i), bounds_.y, seg_w, bounds_.h};
        const bool sel = (static_cast<int>(i) == selected);
        if (sel) {
            r.fill_round(seg.inset(2.0f), Theme::kCornerFull, th.palette.secondary_container);
        } else if (static_cast<int>(i) == hover_) {
            r.fill_round(seg.inset(2.0f), Theme::kCornerFull,
                         th.palette.on_surface.alpha_of(Theme::kStateHover));
        }
        r.text_line(items[i], seg, TypeStyle::LabelLarge,
                    sel ? th.palette.on_secondary_container : th.palette.on_surface_variant,
                    TextAlign::Center, true);
    }
}

bool SegmentedTabs::on_pointer_down(const Point& p) {
    if (items.empty() || !bounds_.contains(p)) return false;
    const float seg_w = bounds_.w / static_cast<float>(items.size());
    int idx = static_cast<int>((p.x - bounds_.x) / std::max(1.0f, seg_w));
    idx = std::clamp(idx, 0, static_cast<int>(items.size()) - 1);
    if (idx != selected) {
        selected = idx;
        if (on_change) on_change(idx);
    }
    return true;
}

bool SegmentedTabs::on_pointer_move(const Point& p, bool inside) {
    if (!inside || items.empty()) { hover_ = -1; return true; }
    const float seg_w = bounds_.w / static_cast<float>(items.size());
    hover_ = std::clamp(static_cast<int>((p.x - bounds_.x) / std::max(1.0f, seg_w)), 0,
                        static_cast<int>(items.size()) - 1);
    return true;
}

// -----------------------------------------------------------------------------
//  NavBar
// -----------------------------------------------------------------------------

void NavBar::paint(Renderer& r, const Theme& th) {
    if (items.empty() || bounds_.empty()) return;

    r.fill(bounds_, th.palette.surface_container);
    r.fill({bounds_.x, bounds_.y, bounds_.w, 1.0f}, th.palette.outline_variant.alpha_of(0.6f));

    const float seg_w = bounds_.w / static_cast<float>(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const Rect seg{bounds_.x + seg_w * static_cast<float>(i), bounds_.y, seg_w, bounds_.h};
        const bool sel = (static_cast<int>(i) == selected);

        // 选中项背后的 pill（M3 3.x 的 navigation bar 规范）
        if (sel) {
            const Rect pill{seg.center_x() - 32.0f, seg.y + 8.0f, 64.0f, 32.0f};
            r.fill_round(pill, Theme::kCornerFull, th.palette.secondary_container);
        } else if (static_cast<int>(i) == hover_) {
            const Rect pill{seg.center_x() - 32.0f, seg.y + 8.0f, 64.0f, 32.0f};
            r.fill_round(pill, Theme::kCornerFull, th.palette.on_surface.alpha_of(Theme::kStateHover));
        }

        r.text_line(items[i].glyph, {seg.x, seg.y + 8.0f, seg.w, 32.0f}, TypeStyle::TitleMedium,
                    sel ? th.palette.on_secondary_container : th.palette.on_surface_variant,
                    TextAlign::Center, false);
        r.text_line(items[i].label, {seg.x, seg.y + 42.0f, seg.w, 18.0f}, TypeStyle::LabelMedium,
                    sel ? th.palette.on_surface : th.palette.on_surface_variant,
                    TextAlign::Center, true);
    }
}

bool NavBar::on_pointer_down(const Point& p) {
    if (items.empty() || !bounds_.contains(p)) return false;
    const float seg_w = bounds_.w / static_cast<float>(items.size());
    int idx = std::clamp(static_cast<int>((p.x - bounds_.x) / std::max(1.0f, seg_w)), 0,
                         static_cast<int>(items.size()) - 1);
    if (idx != selected) {
        selected = idx;
        if (on_change) on_change(idx);
    }
    return true;
}

bool NavBar::on_pointer_move(const Point& p, bool inside) {
    if (!inside || items.empty()) { hover_ = -1; return true; }
    const float seg_w = bounds_.w / static_cast<float>(items.size());
    hover_ = std::clamp(static_cast<int>((p.x - bounds_.x) / std::max(1.0f, seg_w)), 0,
                        static_cast<int>(items.size()) - 1);
    return true;
}

// -----------------------------------------------------------------------------
//  TextField
// -----------------------------------------------------------------------------

float TextField::preferred_height(float, Renderer& r) {
    const float field_h = multiline ? 108.0f : 56.0f;
    field_height_ = field_h;
    float extra = 0.0f;
    if (!error.empty()) {
        extra = std::max(18.0f, r.measure(error, TypeStyle::BodySmall, 400.0f, true).h) + 4.0f;
    } else if (!supporting.empty()) {
        extra = std::max(18.0f, r.measure(supporting, TypeStyle::BodySmall, 400.0f, true).h) + 4.0f;
    }
    return field_h + extra;
}

void TextField::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty()) return;

    const bool bad = !error.empty();
    field_rect_ = {bounds_.x, bounds_.y, bounds_.w, field_height_};

    // M3 的 filled text field：容器色 + 底部一条 1px/2px 的指示线
    const Color bg = bad ? th.palette.error_container.alpha_of(th.is_dark ? 0.28f : 0.45f)
                         : th.palette.surface_container_highest;
    r.fill_round(field_rect_, Theme::kCornerSm, bg);

    if (hover_ && !focused) {
        r.fill_round(field_rect_, Theme::kCornerSm,
                     th.palette.on_surface.alpha_of(Theme::kStateHover * 0.6f));
    }

    const Color indicator = bad ? th.palette.error
                                : (focused ? th.palette.primary : th.palette.on_surface_variant);
    const float ind_h = focused ? 2.0f : 1.0f;
    r.fill({field_rect_.x + 1.0f, field_rect_.bottom() - ind_h, field_rect_.w - 2.0f, ind_h}, indicator);

    // 前缀（金额的 ¥）
    float text_x = field_rect_.x + 16.0f;
    if (!prefix.empty()) {
        const Size pm = r.measure(prefix, style, 60.0f, false);
        r.text_line(prefix, {text_x, field_rect_.y, pm.w + 2.0f, field_rect_.h}, style,
                    th.palette.on_surface_variant, TextAlign::Left, false);
        text_x += pm.w + 6.0f;
    }

    const float text_w = std::max(10.0f, field_rect_.right() - 16.0f - text_x);

    // 内容（密码显示成圆点）
    std::wstring shown;
    if (password) {
        shown.assign(value.size(), L'\u2022');
    } else {
        shown = value;
    }

    const bool empty = value.empty();
    const std::wstring display = empty ? placeholder : shown;

    // 选区高亮必须在文字之前画，否则会给文字蒙上一层色。
    // 宽度一律用 shown 量（而不是 value）：密码框里一个字符显示成一个圆点，
    // 用原文测量会算出一段比实际长一截的高亮。
    if (focused && !empty && has_selection()) {
        const size_t b = sel_begin();
        const std::wstring pre = shown.substr(0, b);
        const std::wstring mid = shown.substr(b, sel_end() - b);
        const float pre_w = pre.empty() ? 0.0f : r.measure(pre, style, 1.0e6f, false).w;
        const float mid_w = r.measure(mid, style, 1.0e6f, false).w;
        if (mid_w > 0.0f) {
            r.fill({text_x + pre_w, field_rect_.y + 5.0f, mid_w, field_rect_.h - 10.0f},
                   th.palette.primary.alpha_of(0.30f));
        }
    }

    const Color text_color = empty ? th.palette.on_surface_variant.alpha_of(0.7f)
                                   : (bad ? th.palette.on_error_container : th.palette.on_surface);
    r.text_line(display, {text_x, field_rect_.y, text_w, field_rect_.h}, style, text_color,
                TextAlign::Left, !empty);

    // 光标：只在有焦点时画，位置按「光标前的文本」实测宽度算，不是估算
    if (focused && caret_on_) {
        const std::wstring before = shown.substr(0, std::min(caret_, shown.size()));
        const float caret_dx = before.empty() ? 0.0f : r.measure(before, style, 100000.0f, false).w;
        const float cx = std::min(text_x + caret_dx, field_rect_.right() - 18.0f);
        const float ch = multiline ? (field_rect_.h / 3.0f) : field_rect_.h;
        const float cy = multiline ? (field_rect_.y + (field_rect_.h - ch) * 0.5f) : field_rect_.y;
        r.fill({cx, cy + ch * 0.22f, focused ? 2.0f : 1.0f, ch * 0.56f}, th.palette.primary);
    }

    // 辅助/错误文字
    const float msg_y = field_rect_.bottom() + 4.0f;
    if (bad) {
        r.text(error, {bounds_.x + 16.0f, msg_y, bounds_.w - 32.0f, bounds_.bottom() - msg_y},
               TypeStyle::BodySmall, th.palette.error, TextAlign::Left, VAlign::Top, true, true);
    } else if (!supporting.empty()) {
        r.text(supporting, {bounds_.x + 16.0f, msg_y, bounds_.w - 32.0f, bounds_.bottom() - msg_y},
               TypeStyle::BodySmall, th.palette.on_surface_variant, TextAlign::Left, VAlign::Top, true, true);
    }
}

bool TextField::on_pointer_down(const Point& p) {
    if (!bounds_.contains(p)) return false;
    // 定位到末尾：真正的「点哪插哪」需要逐字符测量后二分，
    // 对这个应用（金额、商品名、备注）价值不大，先按末尾处理。
    caret_ = value.size();
    caret_on_ = true;
    return true;
}

bool TextField::on_key(unsigned vk, bool down) {
    if (!down || !enabled) return false;

    // Shift 直接问系统（不像 Ctrl —— 那个由 shell 维护状态，因为诊断模式要能注入）。
    const bool shift = key_down(key::Shift);

    switch (vk) {
        case key::Backspace: {
            bool changed = delete_selection();
            if (!changed && caret_ > 0 && !value.empty()) {
                value.erase(caret_ - 1, 1);
                --caret_;
                changed = true;
            }
            if (changed) {
                anchor_ = caret_;
                after_change();
            }
            return true;
        }
        case key::Delete: {
            bool changed = delete_selection();
            if (!changed && caret_ < value.size()) {
                value.erase(caret_, 1);
                changed = true;
            }
            if (changed) {
                anchor_ = caret_;
                after_change();
            }
            return true;
        }
        case key::Left:
            if (shift) {
                if (caret_ > 0) --caret_;      // 锚点不动 → 选区扩大
            } else if (has_selection()) {
                caret_ = sel_begin();          // 不带 Shift 时先「收起」到一端
                anchor_ = caret_;
            } else if (caret_ > 0) {
                --caret_;
            }
            caret_on_ = true;
            return true;
        case key::Right:
            if (shift) {
                if (caret_ < value.size()) ++caret_;
            } else if (has_selection()) {
                caret_ = sel_end();
                anchor_ = caret_;
            } else if (caret_ < value.size()) {
                ++caret_;
            }
            caret_on_ = true;
            return true;
        case key::Home:
            caret_ = 0;
            if (!shift) anchor_ = 0;
            caret_on_ = true;
            return true;
        case key::End:
            caret_ = value.size();
            if (!shift) anchor_ = caret_;
            caret_on_ = true;
            return true;
        case key::Enter:
            if (!multiline && on_submit) on_submit();
            return true;
        default:
            break;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  选区
// -----------------------------------------------------------------------------

bool TextField::has_selection() const { return anchor_ != caret_; }

size_t TextField::sel_begin() const { return std::min(anchor_, caret_); }
size_t TextField::sel_end() const { return std::max(anchor_, caret_); }

std::wstring TextField::selected_text() const {
    if (!has_selection()) return {};
    const size_t b = sel_begin();
    return value.substr(b, sel_end() - b);
}

void TextField::select_all() {
    anchor_ = 0;
    caret_ = value.size();
    caret_on_ = true;
}

bool TextField::delete_selection() {
    if (!has_selection()) return false;
    const size_t b = sel_begin();
    value.erase(b, sel_end() - b);
    caret_ = b;
    anchor_ = b;
    return true;
}

// -----------------------------------------------------------------------------
//  校验与插入
// -----------------------------------------------------------------------------

/// 注意这里是按「整串」判断，而不是像逐字符输入那样逐字符判断。
/// 差别在粘贴：粘进来的是一整串，只有整串校验才能一次性判断
/// 「12.345」该不该被接受（三位小数 → 不接受）。
bool TextField::accepts(const std::wstring& s) const {
    if (s.size() > kMaxTextLength) return false;
    if (!numeric) return true;

    bool  dot_seen = false;
    size_t dot_at = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'.') {
            if (dot_seen) return false;
            dot_seen = true;
            dot_at = i;
        } else if (c < L'0' || c > L'9') {
            return false;
        }
    }
    // 最多两位小数
    if (dot_seen && s.size() - dot_at - 1 > 2) return false;
    return true;
}

void TextField::after_change() {
    caret_on_ = true;
    if (on_change) on_change();
}

bool TextField::insert_text(const std::wstring& text) {
    if (text.empty()) return false;
    std::wstring t = text;

    if (numeric && !accepts(t)) {
        // 从截图里复制的常常是「¥12.34」这种带装饰的串。先试着把数字和小数点
        // 挑出来再判一次；挑完还是不合法（比如「abc」、三位小数）就整体拒绝 ——
        // 静默把内容改掉比拒绝更坏，用户会拿着一份被悄悄改过的数据去对账。
        std::wstring cleaned;
        bool dot_seen = false;
        for (const wchar_t c : t) {
            if (c >= L'0' && c <= L'9') {
                cleaned.push_back(c);
            } else if (c == L'.' && !dot_seen) {
                cleaned.push_back(c);
                dot_seen = true;
            }
        }
        if (cleaned.empty() || !accepts(cleaned)) return false;
        t = std::move(cleaned);
    }

    const size_t from = has_selection() ? sel_begin() : std::min(caret_, value.size());
    const size_t to = has_selection() ? sel_end() : from;

    const size_t kept = value.size() - (to - from);
    if (kept >= kMaxTextLength) return false;
    if (t.size() > kMaxTextLength - kept) t.resize(kMaxTextLength - kept);

    value.replace(from, to - from, t);
    caret_ = from + t.size();
    anchor_ = caret_;
    after_change();
    return true;
}

bool TextField::on_edit_command(EditCmd cmd) {
    if (!enabled) return false;

    switch (cmd) {
        case EditCmd::SelectAll:
            select_all();
            return true;

        case EditCmd::Copy: {
            const std::wstring sel = selected_text();
            if (!sel.empty()) clipboard_write_text(sel);
            // 没选中也返回 true：这条按键已被输入框消费，不该再冒泡出去
            return true;
        }

        case EditCmd::Cut: {
            const std::wstring sel = selected_text();
            if (sel.empty()) return true;
            clipboard_write_text(sel);
            if (delete_selection()) after_change();
            return true;
        }

        case EditCmd::Paste: {
            // 密码框也允许粘贴 —— 这是刻意的。这个应用的密码框还有一层用途：
            // 填 API Key，而那串东西又长又随机，禁止粘贴等于逼用户手抄。
            const std::wstring raw = clipboard_read_text();
            if (raw.empty()) return true;
            insert_text(multiline ? raw : sanitize_single_line(raw));
            return true;
        }
    }
    return false;
}

bool TextField::on_text(wchar_t ch) {
    if (!enabled) return false;
    if (ch < 32) return true;   // 控制字符不进输入（Ctrl+V 产生的 0x16 也在这里被挡掉）

    // 有选区时，输入是「替换选区」而不是「插入」——这是文本框的通用语义。
    // 做法是先拼出候选串再校验，而不是先删再插：
    // 后者在 numeric 框上会留下「校验失败但选区已经删了」的半截状态。
    const size_t from = has_selection() ? sel_begin() : std::min(caret_, value.size());
    const size_t to = has_selection() ? sel_end() : from;

    std::wstring candidate = value;
    candidate.replace(from, to - from, 1, ch);
    if (!accepts(candidate)) return true;   // 消费掉，但内容原样不动

    value = std::move(candidate);
    caret_ = from + 1;
    anchor_ = caret_;
    after_change();
    return true;
}

void TextField::on_focus_lost() {
    caret_on_ = false;
}

// -----------------------------------------------------------------------------
//  BarChart
// -----------------------------------------------------------------------------

void BarChart::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty() || values.empty()) return;

    const float label_h = labels.empty() ? 0.0f : 20.0f;
    const Rect plot{bounds_.x, bounds_.y, bounds_.w, std::max(1.0f, bounds_.h - label_h)};

    const Color base = color_unset(bar) ? th.palette.primary : bar;
    const Color hi = color_unset(bar_alt) ? th.palette.error : bar_alt;

    const float n = static_cast<float>(values.size());
    const float slot = plot.w / n;
    const float bar_w = std::max(2.0f, std::min(slot * 0.62f, 22.0f));
    const float radius = std::min(Theme::kCornerXs, bar_w * 0.5f);

    // 基线
    r.fill({plot.x, plot.bottom() - 1.0f, plot.w, 1.0f}, th.palette.outline_variant.alpha_of(0.7f));

    for (size_t i = 0; i < values.size(); ++i) {
        const float v = std::clamp(values[i], 0.0f, 1.0f);
        // 0 值也画 2px，让「这天没花钱」和「数据缺失」在视觉上可区分
        const float h = std::max(2.0f, v * (plot.h - 6.0f));
        const float x = plot.x + slot * static_cast<float>(i) + (slot - bar_w) * 0.5f;
        const float y = plot.bottom() - 1.0f - h;
        const Color c = (static_cast<int>(i) == highlight_last) ? hi : base;
        r.fill_round({x, y, bar_w, h}, radius, c);
    }

    if (!labels.empty()) {
        // 标签要按宽度抽稀。「一格里塞不下 8/23 还硬画」的结果是相邻标签叠在一起 ——
        // 30 天数据在窄窗口里每格只有十几像素，而一个日期要三十多。
        constexpr float kLabelNeed = 38.0f;   // 12px 下「8/23」的宽度（含余量）
        int step = 1;
        if (slot > 0.0f) {
            step = std::max(1, static_cast<int>(std::ceil((kLabelNeed + 6.0f) / slot)));
        }
        for (size_t i = 0; i < labels.size() && i < values.size();
             i += static_cast<size_t>(step)) {
            // 框宽固定为文字需要的宽度，**不能是 slot**：
            // 文本排版会按框裁剪，框只有一格宽的时候「8/23」会被裁成「8/2」——
            // 看起来像日期算错了，实际上只是画不下。
            const float cx = plot.x + slot * (static_cast<float>(i) + 0.5f);
            Rect lb{cx - kLabelNeed * 0.5f, plot.bottom(), kLabelNeed, label_h};
            // 首尾夹在绘图区内，避免第一个标签向左越界
            if (lb.x < plot.x) lb.x = plot.x;
            if (lb.x + lb.w > plot.right()) lb.x = plot.right() - lb.w;
            r.text_line(labels[i], lb, TypeStyle::BodySmall,
                        th.palette.on_surface_variant, TextAlign::Center, false);
        }
    }
}

// -----------------------------------------------------------------------------
//  ShareBar
// -----------------------------------------------------------------------------

void ShareBar::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty() || segments.empty()) return;

    r.fill_round(bounds_, height * 0.5f, th.palette.surface_container_highest);

    float total = 0.0f;
    for (const auto& s : segments) total += std::max(0.0f, s.weight);
    if (total <= 0.0f) return;

    r.push_clip(bounds_);
    float x = bounds_.x;
    for (const auto& s : segments) {
        const float w = bounds_.w * (std::max(0.0f, s.weight) / total);
        if (w <= 0.0f) continue;
        // 段与段之间留 2px 白缝，否则同色相邻的两段会糊成一块
        r.fill_round({x, bounds_.y, std::max(1.0f, w - 2.0f), bounds_.h}, height * 0.5f, s.color);
        x += w;
    }
    r.pop_clip();
}

// -----------------------------------------------------------------------------
//  CategoryBar
// -----------------------------------------------------------------------------

void CategoryBar::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty()) return;

    const Color c = color_unset(color) ? th.palette.primary : color;
    const float dot_x = bounds_.x + 5.0f;

    r.fill_circle(dot_x, bounds_.y + 14.0f, 5.0f, c);
    r.text_line(name, {dot_x + 14.0f, bounds_.y + 2.0f, bounds_.w * 0.5f, 24.0f},
                TypeStyle::BodyMedium, th.palette.on_surface, TextAlign::Left, true);
    r.text_line(amount, {bounds_.x, bounds_.y + 2.0f, bounds_.w - 84.0f, 24.0f},
                TypeStyle::BodyMedium, th.palette.on_surface, TextAlign::Right, true);
    r.text_line(std::to_wstring(static_cast<int>(share * 100.0f + 0.5f)) + L"%",
                {bounds_.right() - 64.0f, bounds_.y + 2.0f, 64.0f, 24.0f}, TypeStyle::BodySmall,
                th.palette.on_surface_variant, TextAlign::Right, false);

    // 细进度条
    const Rect track{bounds_.x, bounds_.y + 30.0f, bounds_.w, 6.0f};
    r.fill_round(track, 3.0f, th.palette.surface_container_highest);
    const float w = std::max(0.0f, bounds_.w * std::clamp(share, 0.0f, 1.0f));
    if (w > 0.5f) r.fill_round({track.x, track.y, w, track.h}, 3.0f, c);
}

// -----------------------------------------------------------------------------
//  Snackbar
// -----------------------------------------------------------------------------

void Snackbar::layout(const Rect& area, Renderer&) {
    const float w = std::min(560.0f, std::max(200.0f, area.w - 32.0f));
    const float h = 56.0f;
    bounds_ = {area.x + (area.w - w) * 0.5f, area.bottom() - h - 24.0f, w, h};
}

void Snackbar::paint(Renderer& r, const Theme& th) {
    const Color bg = is_error ? th.palette.error_container : th.palette.inverse_surface;
    const Color fg = is_error ? th.palette.on_error_container : th.palette.inverse_on_surface;
    r.fill_round(bounds_, Theme::kCornerSm, bg);
    r.text(text, {bounds_.x + 16.0f, bounds_.y, bounds_.w - 32.0f, bounds_.h}, TypeStyle::BodyMedium,
           fg, TextAlign::Left, VAlign::Middle, true, true);
}

// -----------------------------------------------------------------------------
//  ProgressBar
// -----------------------------------------------------------------------------

void ProgressBar::paint(Renderer& r, const Theme& th) {
    if (bounds_.empty()) return;

    const float radius = bounds_.h * 0.5f;
    r.fill_round(bounds_, radius, th.palette.surface_container_highest);

    if (value >= 0.0f) {
        const float w = bounds_.w * std::clamp(value, 0.0f, 1.0f);
        if (w > 0.5f) r.fill_round({bounds_.x, bounds_.y, w, bounds_.h}, radius, th.palette.primary);
        return;
    }

    // 不确定进度：一段 30% 宽的滑块来回跑
    const float seg = bounds_.w * 0.30f;
    const float travel = bounds_.w - seg;
    const float x = bounds_.x + travel * phase_;
    r.push_clip(bounds_);
    r.fill_round({x, bounds_.y, seg, bounds_.h}, radius, th.palette.primary);
    r.pop_clip();
}

// -----------------------------------------------------------------------------
//  Switch
// -----------------------------------------------------------------------------

Rect Switch::track_rect() const {
    return {bounds_.x, bounds_.y + (bounds_.h - kTrackH) * 0.5f, kTrackW, kTrackH};
}

void Switch::layout(const Rect& area, Renderer&) {
    bounds_ = area;
    // 首次布局时把动画进度对齐到当前值，否则初始状态会从 0 滑过去
    if (!anim_init_) {
        anim_ = value ? 1.0f : 0.0f;
        anim_init_ = true;
    }
}

bool Switch::tick(float dt_ms) {
    const float target = value ? 1.0f : 0.0f;
    if (std::fabs(target - anim_) < 0.005f) {
        if (anim_ != target) {
            anim_ = target;
            return true;
        }
        return false;
    }
    // 时间常数 40ms：约 120ms 走完，开关这种小动作不该慢慢悠悠
    anim_ += (target - anim_) * (1.0f - std::exp(-std::max(0.0f, dt_ms) / 40.0f));
    return true;
}

void Switch::paint(Renderer& r, const Theme& th) {
    const Rect track = track_rect();
    const float radius = kTrackH * 0.5f;
    const bool dis = !enabled;

    // 轨道：关 = surface_container_highest（带描边），开 = primary
    Color track_col = th.palette.surface_container_highest.mix(th.palette.primary, anim_);
    if (dis) track_col = track_col.alpha_of(0.38f);
    r.fill_round(track, radius, track_col);
    if (anim_ < 0.5f) {
        // 关闭态画一圈描边：surface_container_highest 和页面底色很接近，
        // 不描边的话「这里有个开关」几乎看不出来
        r.stroke_round(track, radius, th.palette.outline, 2.0f);
    }
    if (hover_ && enabled) {
        r.fill_round(track, radius, th.palette.on_surface.alpha_of(Theme::kStateHover));
    }

    // 滑块：关 20dp / 开 26dp，位置随 anim_ 移动
    const float thumb_d = 20.0f + 6.0f * anim_;
    const float pad = 6.0f;
    const float cx = track.x + pad + thumb_d * 0.5f +
                     (kTrackW - pad * 2.0f - thumb_d) * anim_;
    const Color thumb_col = th.palette.outline.mix(th.palette.on_primary, anim_);
    r.fill_circle(cx, track.y + track.h * 0.5f, thumb_d * 0.5f,
                  dis ? thumb_col.alpha_of(0.6f) : thumb_col);
}

bool Switch::on_pointer_down(const Point& p) {
    if (!enabled) return false;
    if (!bounds_.contains(p)) return false;
    pressed_ = true;
    return true;
}

bool Switch::on_pointer_move(const Point&, bool inside) {
    hover_ = inside;
    return false;   // 不消费：让父容器照常处理滚动
}

bool Switch::on_pointer_up(const Point& p) {
    const bool was = pressed_;
    pressed_ = false;
    if (!was) return false;
    // 按下后拖出去松手 = 取消（和按钮一致的行为）
    if (!bounds_.contains(p)) return false;
    value = !value;
    if (on_change) on_change(value);
    return true;
}

bool Switch::on_key(unsigned vk, bool down) {
    if (!down || !enabled) return false;
    if (vk == key::Space || vk == key::Enter) {
        value = !value;
        if (on_change) on_change(value);
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  NavRail
// -----------------------------------------------------------------------------

NavRail::NavRail() {
    // HBox 按 width_hint 分配宽度（不参与平分），所以宽度动画就是改这个值：
    // tick 里改一下，下一帧布局自然跟上，不需要额外的重排机制。
    width_hint = anim_w_;
}

float NavRail::preferred_height(float, Renderer&) {
    // 高度由父容器（HBox + Stretch）给，这里只报「内容需要多少」
    float h = 8.0f + kToggleH + 8.0f;
    int body = 0, pinned = 0;
    for (const Item& it : items) (it.pinned_bottom ? pinned : body)++;
    h += static_cast<float>(body) * (kItemH + 4.0f);
    h += static_cast<float>(pinned) * (kItemH + 4.0f) + 16.0f;
    return h;
}

Rect NavRail::item_rect(int index) const {
    if (index < 0 || index >= static_cast<int>(rects_.size())) return {};
    return rects_[static_cast<size_t>(index)];
}

void NavRail::layout(const Rect& area, Renderer&) {
    bounds_ = area;
    if (!anim_init_) {
        anim_w_ = expanded ? kExpandedW : kCollapsedW;
        width_hint = anim_w_;
        anim_init_ = true;
    }

    float y = area.y + 8.0f;
    toggle_rect_ = {area.x + 8.0f, y, area.w - 16.0f, kToggleH};
    y += kToggleH + 8.0f;

    rects_.assign(items.size(), Rect{});

    // 主体项从上往下排
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].pinned_bottom) continue;
        rects_[i] = {area.x + 8.0f, y, area.w - 16.0f, kItemH};
        y += kItemH + 4.0f;
    }
    // 贴底的项从下往上排（设置在最下面）
    float by = area.bottom() - 12.0f;
    for (size_t i = items.size(); i-- > 0;) {
        if (!items[i].pinned_bottom) continue;
        by -= kItemH;
        rects_[i] = {area.x + 8.0f, by, area.w - 16.0f, kItemH};
        by -= 6.0f;
    }
}

bool NavRail::tick(float dt_ms) {
    const float target = expanded ? kExpandedW : kCollapsedW;
    if (std::fabs(target - anim_w_) < 0.5f) {
        if (anim_w_ != target) {
            anim_w_ = target;
            width_hint = anim_w_;
            return true;
        }
        return false;
    }
    // 时间常数 60ms：展开/折叠要有点过渡，但不该让人等
    anim_w_ += (target - anim_w_) * (1.0f - std::exp(-std::max(0.0f, dt_ms) / 60.0f));
    width_hint = anim_w_;
    return true;
}

void NavRail::paint(Renderer& r, const Theme& th) {
    // 和顶栏同色（surface_container），两者合成一个「框架」；
    // 页面内容区用更低的 surface。分区靠容器色，不靠阴影。
    r.fill(bounds_, th.palette.surface_container);
    r.fill({bounds_.right() - 1.0f, bounds_.y, 1.0f, bounds_.h}, th.palette.outline_variant);

    // ---- 折叠按钮 ----
    if (hover_ == -2) {
        r.fill_round(toggle_rect_, Theme::kCornerSm,
                     th.palette.on_surface.alpha_of(Theme::kStateHover));
    }
    // 箭头指向「点了之后会往哪去」：展开时指向左（收起），折叠时指向右（展开）
    r.text(expanded ? L"◀" : L"▶", toggle_rect_, TypeStyle::BodyMedium,
           th.palette.on_surface_variant, TextAlign::Center, VAlign::Middle, false, false);

    // ---- 分隔线：设置与主功能区之间 ----
    int first_pinned = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].pinned_bottom) {
            first_pinned = static_cast<int>(i);
            break;
        }
    }
    if (first_pinned >= 0 && !rects_.empty()) {
        const Rect& pr = rects_[static_cast<size_t>(first_pinned)];
        const float line_y = pr.y - 8.0f;
        r.fill({bounds_.x + 12.0f, line_y, bounds_.w - 24.0f, 1.0f},
               th.palette.outline_variant);
    }

    // ---- 各项 ----
    for (size_t i = 0; i < items.size(); ++i) {
        const Rect rr = rects_[i];
        if (rr.w <= 0.0f) continue;
        const bool sel = (static_cast<int>(i) == selected);
        const float radius = rr.h * 0.5f;

        if (sel) {
            r.fill_round(rr, radius, th.palette.secondary_container);
        } else if (hover_ == static_cast<int>(i)) {
            r.fill_round(rr, radius, th.palette.on_surface.alpha_of(Theme::kStateHover));
        }

        const Color fg = sel ? th.palette.on_secondary_container : th.palette.on_surface_variant;

        // 图标：折叠时就居中，展开时靠左（保持一列对齐，眼睛不用来回找）
        const float icon_cx = expanded ? rr.x + 26.0f : rr.center_x();
        r.text(items[i].glyph, {icon_cx - 16.0f, rr.y, 32.0f, rr.h}, TypeStyle::TitleMedium,
               fg, TextAlign::Center, VAlign::Middle, false, false);

        if (expanded) {
            r.text(items[i].label, {rr.x + 52.0f, rr.y, std::max(20.0f, rr.w - 60.0f), rr.h},
                   TypeStyle::BodyMedium, fg, TextAlign::Left, VAlign::Middle, false, true);
        }
    }
}

bool NavRail::on_pointer_move(const Point& p, bool inside) {
    int h = -1;
    if (inside) {
        if (toggle_rect_.contains(p)) {
            h = -2;
        } else {
            for (size_t i = 0; i < rects_.size(); ++i) {
                if (rects_[i].w > 0.0f && rects_[i].contains(p)) {
                    h = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    if (h != hover_) {
        hover_ = h;
        return true;   // 悬停态变了，需要重绘
    }
    return false;
}

bool NavRail::on_pointer_down(const Point& p) {
    if (!bounds_.contains(p)) return false;
    pressed_ = true;
    return true;
}

bool NavRail::on_pointer_up(const Point& p) {
    const bool was = pressed_;
    pressed_ = false;
    if (!was) return false;

    if (toggle_rect_.contains(p)) {
        if (on_toggle) on_toggle();
        return true;
    }
    for (size_t i = 0; i < rects_.size(); ++i) {
        if (rects_[i].w > 0.0f && rects_[i].contains(p)) {
            if (on_select) on_select(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

}  // namespace penhu::native::ui
