#pragma once
#include <cstdint>
#include <d2d1.h>

namespace sparring {

inline D2D1_COLOR_F color(std::uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

inline D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

inline D2D1_COLOR_F alpha(D2D1_COLOR_F c, float factor) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a * factor);
}

struct Palette {
    bool dark = false;
    D2D1_COLOR_F mica, card, card_stroke, stroke2;
    D2D1_COLOR_F text, text2, text3;
    D2D1_COLOR_F accent, accent_text;
    D2D1_COLOR_F subtle, field, field_line, dash, button_bottom;
    D2D1_COLOR_F ok, warn, warn_fill, error, error_fill;
    // a hue for each kind of thing, so a job, a box and a blueprint never look alike
    D2D1_COLOR_F purple, orange, teal;
    // code coloring
    D2D1_COLOR_F code_keyword, code_type, code_string, code_number, code_comment, code_preproc, code_namespace;
    // the welcome banner
    D2D1_COLOR_F hero_a, hero_b;
};

inline Palette make_palette(bool dark, std::uint32_t accent_rgb) {
    Palette p;
    p.dark = dark;
    if (!dark) {
        p.mica        = color(0xEDF1F9);
        p.card        = color(0xFBFBFB);
        p.card_stroke = color(0x000000, 0.0578f);
        p.stroke2     = color(0x000000, 0.08f);
        p.text        = color(0x1B1B1B);
        p.text2       = color(0x5D5D5D);
        p.text3       = color(0x8A8A8A);
        p.accent      = color(accent_rgb ? accent_rgb : 0x005FB8);
        p.accent_text = color(0xFFFFFF);
        p.subtle      = color(0x000000, 0.0373f);
        p.field       = color(0xFFFFFF);
        p.field_line  = color(0x868686);
        p.dash        = color(0x9A9A9A);
        p.button_bottom = color(0x000000, 0.16f);
        p.ok          = color(0x0F7B0F);
        p.warn        = color(0x9D5D00);
        p.warn_fill   = color(0xFFF4CE);
        p.error       = color(0xC42B1C);
        p.error_fill  = color(0xFDE7E9);
        p.purple      = color(0x7A4EC2);
        p.orange      = color(0xCA5010);
        p.teal        = color(0x038387);
        p.code_keyword   = color(0x0000FF);
        p.code_type      = color(0x2B91AF);
        p.code_string    = color(0xA31515);
        p.code_number    = color(0x098658);
        p.code_comment   = color(0x008000);
        p.code_preproc   = color(0x808080);
        p.code_namespace = color(0x267F99);
        p.hero_a      = color(0x0063B1);
        p.hero_b      = color(0x7A4EC2);
    } else {
        p.mica        = color(0x202328);
        p.card        = color(0x2B2B2B);
        p.card_stroke = color(0xFFFFFF, 0.07f);
        p.stroke2     = color(0xFFFFFF, 0.08f);
        p.text        = color(0xFFFFFF);
        p.text2       = color(0xC5C5C5);
        p.text3       = color(0x9A9A9A);
        p.accent      = color(accent_rgb ? accent_rgb : 0x60CDFF);
        p.accent_text = color(0x000000);
        p.subtle      = color(0xFFFFFF, 0.06f);
        p.field       = color(0xFFFFFF, 0.06f);
        p.field_line  = color(0x9A9A9A);
        p.dash        = color(0x6F6F6F);
        p.button_bottom = color(0xFFFFFF, 0.05f);
        p.ok          = color(0x6CCB5F);
        p.warn        = color(0xFCE100);
        p.warn_fill   = color(0x433519);
        p.error       = color(0xFF99A4);
        p.error_fill  = color(0x442726);
        p.purple      = color(0xB79CFF);
        p.orange      = color(0xFF9D5C);
        p.teal        = color(0x4CD6D0);
        p.code_keyword   = color(0x569CD6);
        p.code_type      = color(0x4EC9B0);
        p.code_string    = color(0xD69D85);
        p.code_number    = color(0xB5CEA8);
        p.code_comment   = color(0x6A9955);
        p.code_preproc   = color(0x9B9B9B);
        p.code_namespace = color(0x4EC9B0);
        p.hero_a      = color(0x0F5FA8);
        p.hero_b      = color(0x5B3A9E);
    }
    return p;
}

}  // namespace sparring
