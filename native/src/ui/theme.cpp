// =============================================================================
//  native/ui/theme.cpp
//  M3 设计令牌的具体取值。
//
//  色值直接取自原 web 版 styles.css 的 :root 与 [data-theme="dark"]，
//  一个字节都没换 —— 要求是「界面风格沿用现有设计」，那就不能是我另挑一套
//  「差不多」的颜色，否则深浅两套主题、图表配色、支出/收入语义色会各自漂移。
// =============================================================================

#include "ui/foundation.hpp"

namespace penhu::native::ui {

Color Color::from_hex(const std::string& hex, Color fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = hex[i];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return fallback;
        v = (v << 4) | d;
    }
    return Color::from_rgb(v);
}

Theme Theme::light() {
    Theme t;
    t.is_dark = false;
    Palette& p = t.palette;

    p.primary                  = Color::from_rgb(0x6750A4);
    p.on_primary               = Color::from_rgb(0xFFFFFF);
    p.primary_container        = Color::from_rgb(0xEADDFF);
    p.on_primary_container     = Color::from_rgb(0x21005D);

    p.secondary                = Color::from_rgb(0x625B71);
    p.on_secondary             = Color::from_rgb(0xFFFFFF);
    p.secondary_container      = Color::from_rgb(0xE8DEF8);
    p.on_secondary_container   = Color::from_rgb(0x1D192B);

    p.tertiary                 = Color::from_rgb(0x7D5260);
    p.on_tertiary              = Color::from_rgb(0xFFFFFF);
    p.tertiary_container       = Color::from_rgb(0xFFD8E4);
    p.on_tertiary_container    = Color::from_rgb(0x31111D);

    p.error                    = Color::from_rgb(0xB3261E);
    p.on_error                 = Color::from_rgb(0xFFFFFF);
    p.error_container          = Color::from_rgb(0xF9DEDC);
    p.on_error_container       = Color::from_rgb(0x410E0B);

    p.surface                  = Color::from_rgb(0xFEF7FF);
    p.on_surface               = Color::from_rgb(0x1D1B20);
    p.surface_variant          = Color::from_rgb(0xE7E0EC);
    p.on_surface_variant       = Color::from_rgb(0x49454F);

    p.surface_container_lowest = Color::from_rgb(0xFFFFFF);
    p.surface_container_low    = Color::from_rgb(0xF7F2FA);
    p.surface_container        = Color::from_rgb(0xF3EDF7);
    p.surface_container_high   = Color::from_rgb(0xECE6F0);
    p.surface_container_highest= Color::from_rgb(0xE6E0E9);

    p.outline                  = Color::from_rgb(0x79747E);
    p.outline_variant          = Color::from_rgb(0xCAC4D0);
    p.inverse_surface          = Color::from_rgb(0x322F35);
    p.inverse_on_surface       = Color::from_rgb(0xF5EFF7);
    p.scrim                    = Color::from_rgb(0x000000);

    p.expense                  = Color::from_rgb(0xB3261E);
    p.income                   = Color::from_rgb(0x2E6B34);
    p.good                     = Color::from_rgb(0x2E6B34);
    p.warn                     = Color::from_rgb(0xA15C00);
    return t;
}

Theme Theme::dark() {
    Theme t;
    t.is_dark = true;
    Palette& p = t.palette;

    p.primary                  = Color::from_rgb(0xD0BCFF);
    p.on_primary               = Color::from_rgb(0x381E72);
    p.primary_container        = Color::from_rgb(0x4F378B);
    p.on_primary_container     = Color::from_rgb(0xEADDFF);

    p.secondary                = Color::from_rgb(0xCCC2DC);
    p.on_secondary             = Color::from_rgb(0x332D41);
    p.secondary_container      = Color::from_rgb(0x4A4458);
    p.on_secondary_container   = Color::from_rgb(0xE8DEF8);

    p.tertiary                 = Color::from_rgb(0xEFB8C8);
    p.on_tertiary              = Color::from_rgb(0x492532);
    p.tertiary_container       = Color::from_rgb(0x633B48);
    p.on_tertiary_container    = Color::from_rgb(0xFFD8E4);

    p.error                    = Color::from_rgb(0xF2B8B5);
    p.on_error                 = Color::from_rgb(0x601410);
    p.error_container          = Color::from_rgb(0x8C1D18);
    p.on_error_container       = Color::from_rgb(0xF9DEDC);

    p.surface                  = Color::from_rgb(0x141218);
    p.on_surface               = Color::from_rgb(0xE6E0E9);
    p.surface_variant          = Color::from_rgb(0x49454F);
    p.on_surface_variant       = Color::from_rgb(0xCAC4D0);

    p.surface_container_lowest = Color::from_rgb(0x0F0D13);
    p.surface_container_low    = Color::from_rgb(0x1D1B20);
    p.surface_container        = Color::from_rgb(0x211F26);
    p.surface_container_high   = Color::from_rgb(0x2B2930);
    p.surface_container_highest= Color::from_rgb(0x36343B);

    p.outline                  = Color::from_rgb(0x938F99);
    p.outline_variant          = Color::from_rgb(0x49454F);
    p.inverse_surface          = Color::from_rgb(0xE6E0E9);
    p.inverse_on_surface       = Color::from_rgb(0x322F35);
    p.scrim                    = Color::from_rgb(0x000000);

    p.expense                  = Color::from_rgb(0xF2B8B5);
    p.income                   = Color::from_rgb(0xA6D8A8);
    p.good                     = Color::from_rgb(0xA6D8A8);
    p.warn                     = Color::from_rgb(0xF5C77E);
    return t;
}

TypeToken Theme::type(TypeStyle style) const {
    switch (style) {
        case TypeStyle::DisplaySmall:  return {36.0f, 44.0f, 400};
        case TypeStyle::HeadlineSmall: return {24.0f, 32.0f, 400};
        case TypeStyle::TitleLarge:    return {22.0f, 28.0f, 400};
        case TypeStyle::TitleMedium:   return {16.0f, 24.0f, 500};
        case TypeStyle::TitleSmall:    return {14.0f, 20.0f, 500};
        case TypeStyle::BodyLarge:     return {16.0f, 24.0f, 400};
        case TypeStyle::BodyMedium:    return {14.0f, 20.0f, 400};
        // 12px 的中文在 150% 缩放下偏小、发虚 —— 全应用的小字提到 13。
        // 行高同步给到 18，中文行距过紧会显得挤。
        case TypeStyle::BodySmall:     return {13.0f, 18.0f, 400};
        case TypeStyle::LabelLarge:    return {14.0f, 20.0f, 500};
        case TypeStyle::LabelMedium:   return {13.0f, 18.0f, 500};
    }
    return {14.0f, 20.0f, 400};
}

}  // namespace penhu::native::ui
