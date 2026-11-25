#include "render.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "explain.hpp"
#include "examples.hpp"
#include "lessons.hpp"
#include "mutate.hpp"
#include "progress.hpp"
#include "quest.hpp"
#include "quiz.hpp"

namespace sparring {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool Renderer::init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))))
        return false;
    ComPtr<IDWriteFontCollection> fonts;
    dwrite->GetSystemFontCollection(&fonts, FALSE);
    auto pick = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const wchar_t* n : names) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (fonts && SUCCEEDED(fonts->FindFamilyName(n, &index, &exists)) && exists) return n;
        }
        return L"Segoe UI";
    };
    body_ = pick({L"Segoe UI Variable Text", L"Segoe UI"});
    // headings: round and friendly when Rubik is there, else Windows' own Bahnschrift
    display_ = pick({L"Rubik", L"Bahnschrift", L"Segoe UI Variable Display", L"Segoe UI"});
    icons_ = pick({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    mono_ = pick({L"Cascadia Mono", L"Consolas", L"Courier New"});

    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
    props.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    const float dashes[] = {4.0f, 3.0f};
    d2d->CreateStrokeStyle(props, dashes, 2, &dash);
    return true;
}

bool Renderer::ensure_target(HWND hwnd, UINT window_dpi) {
    dpi = window_dpi;
    if (target) return true;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right)), static_cast<UINT32>(std::max<LONG>(1, rc.bottom)));
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size), &target))) return false;
    target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    return SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush));
}

void Renderer::resize(UINT width_px, UINT height_px) {
    if (target) target->Resize(D2D1::SizeU(std::max(1u, width_px), std::max(1u, height_px)));
}

void Renderer::set_dpi(UINT window_dpi) {
    dpi = window_dpi;
    if (target) target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

IDWriteTextFormat* Renderer::format(int kind, float size, int weight, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT vertical, bool wrap) {
    const Key key{kind, static_cast<int>(size * 10), weight, static_cast<int>(align), static_cast<int>(vertical), wrap};
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const std::wstring& family = kind == 1 ? display_ : kind == 2 ? icons_ : kind == 3 ? mono_ : body_;
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dwrite->CreateTextFormat(family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(vertical);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
        f->SetTrimming(&trim, ellipsis.Get());
    }
    formats_[key] = f;
    return f.Get();
}

float Renderer::measure(const std::wstring& s, int kind, float size, int weight) {
    IDWriteTextFormat* f = format(kind, size, weight, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 10000, 100, &layout))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Renderer::text_height(const std::wstring& s, int kind, float size, int weight, float width) {
    IDWriteTextFormat* f = format(kind, size, weight, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    ComPtr<IDWriteTextLayout> layout;
    if (s.empty() || !f || FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, std::max(1.0f, width), 10000, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

namespace {

constexpr int kNormal = DWRITE_FONT_WEIGHT_NORMAL;
constexpr int kSemibold = DWRITE_FONT_WEIGHT_SEMI_BOLD;

D2D1_RECT_F rf(const Rect& r) { return D2D1::RectF(r.x, r.y, r.right(), r.bottom()); }

float ease(float t) {
    const float x = std::clamp(t, 0.0f, 1.0f);
    return x * x * (3 - 2 * x);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\t') {
            cur += "    ";
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

// ---- code coloring
enum class Tok { Plain, Keyword, Type, String, Number, Comment, Preproc, Namespace };
struct Span {
    std::size_t begin, end;
    Tok tok;
};

bool ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

Tok word_kind(const std::string& w) {
    static const char* const keywords[] = {"if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default",
                                           "struct", "class", "public", "private", "protected", "const", "constexpr", "static", "true",
                                           "false", "nullptr", "new", "delete", "using", "namespace", "template", "typename", "sizeof",
                                           "enum", "virtual", "override", "this", "operator", "inline", "throw", "try", "catch"};
    static const char* const types[] = {"int", "double", "float", "bool", "char", "void", "long", "short", "unsigned", "signed", "auto",
                                        "string", "size_t", "vector", "map", "cout", "cin", "endl"};
    for (const char* k : keywords)
        if (w == k) return Tok::Keyword;
    for (const char* t : types)
        if (w == t) return Tok::Type;
    if (w == "std") return Tok::Namespace;
    return Tok::Plain;
}

std::vector<Span> lex_line(const std::string& s, bool& in_block) {
    std::vector<Span> out;
    const std::size_t n = s.size();
    auto push = [&](std::size_t b, std::size_t e, Tok t) {
        if (e > b) out.push_back({b, e, t});
    };
    std::size_t i = 0;
    if (!in_block) {
        std::size_t k = 0;
        while (k < n && s[k] == ' ') ++k;
        if (k < n && s[k] == '#') {
            std::size_t e = k + 1;
            while (e < n && std::isalpha(static_cast<unsigned char>(s[e]))) ++e;
            push(0, e, Tok::Preproc);
            std::size_t r = e;
            while (r < n && s[r] == ' ') ++r;
            push(e, r, Tok::Plain);
            push(r, n, r < n && (s[r] == '<' || s[r] == '"') ? Tok::String : Tok::Plain);
            return out;
        }
    }
    std::size_t plain = 0;
    auto flush = [&](std::size_t upto) { push(plain, upto, Tok::Plain); };
    while (i < n) {
        if (in_block) {
            const std::size_t e = s.find("*/", i);
            const std::size_t end = e == std::string::npos ? n : e + 2;
            push(i, end, Tok::Comment);
            in_block = e == std::string::npos;
            i = plain = end;
            continue;
        }
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            flush(i);
            push(i, n, Tok::Comment);
            return out;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            flush(i);
            const std::size_t e = s.find("*/", i + 2);
            const std::size_t end = e == std::string::npos ? n : e + 2;
            push(i, end, Tok::Comment);
            in_block = e == std::string::npos;
            i = plain = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            flush(i);
            std::size_t j = i + 1;
            while (j < n && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            j = std::min(n, j + 1);
            push(i, j, Tok::String);
            i = plain = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) && (i == 0 || !ident(s[i - 1]))) {
            flush(i);
            std::size_t j = i;
            while (j < n && (ident(s[j]) || s[j] == '.')) ++j;
            push(i, j, Tok::Number);
            i = plain = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < n && ident(s[j])) ++j;
            const Tok t = word_kind(s.substr(i, j - i));
            if (t != Tok::Plain) {
                flush(i);
                push(i, j, t);
                plain = j;
            }
            i = j;
            continue;
        }
        ++i;
    }
    flush(n);
    return out;
}

struct Draw {
    Renderer& R;
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* b;
    const Palette& pal;
    const Model& m;
    const Layout& l;

    void fill(const Rect& r, float radius, D2D1_COLOR_F c) {
        if (r.w <= 0 || r.h <= 0) return;
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(rf(r), radius, radius), b);
        else rt->FillRectangle(rf(r), b);
    }
    void stroke(const Rect& r, float radius, D2D1_COLOR_F c, float width = 1.0f, ID2D1StrokeStyle* style = nullptr) {
        if (r.w <= 0 || r.h <= 0) return;
        b->SetColor(c);
        const float h = width / 2;
        const D2D1_RECT_F in = D2D1::RectF(r.x + h, r.y + h, r.right() - h, r.bottom() - h);
        if (radius > 0) rt->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.0f, radius - h), std::max(0.0f, radius - h)), b, width, style);
        else rt->DrawRectangle(in, b, width, style);
    }
    void hline(float x1, float x2, float y, D2D1_COLOR_F c) {
        b->SetColor(c);
        rt->DrawLine(D2D1::Point2F(x1, y + 0.5f), D2D1::Point2F(x2, y + 0.5f), b, 1.0f);
    }
    void circle(const Rect& r, D2D1_COLOR_F c) {
        b->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + r.w / 2, r.y + r.h / 2), r.w / 2, r.h / 2), b);
    }
    // A soft shadow under a raised surface.
    void shadow(const Rect& r, float radius, float strength) {
        const float base = pal.dark ? strength * 2.2f : strength;
        for (int i = 4; i >= 1; --i) {
            const float s = static_cast<float>(i);
            fill(Rect{r.x - s * 0.5f, r.y + s * 0.6f, r.w + s, r.h + s * 0.6f}, radius + s * 0.5f, color(0x000000, base / s));
        }
    }
    void text(const std::wstring& s, const Rect& r, int kind, float size, int weight, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false,
              DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (s.empty() || r.w <= 0 || r.h <= 0) return;
        IDWriteTextFormat* f = R.format(kind, size, weight, align, vertical, wrap);
        if (!f) return;
        b->SetColor(c);
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, rf(r), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void para(const std::string& s, const Rect& r, float size, D2D1_COLOR_F c) {
        text(widen(s), r, 0, size, kNormal, c, DWRITE_TEXT_ALIGNMENT_LEADING, true, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    void glyph(wchar_t g, const Rect& r, float size, D2D1_COLOR_F c) { text(std::wstring(1, g), r, 2, size, kNormal, c, DWRITE_TEXT_ALIGNMENT_CENTER); }
    // A rounded square or circle of a hue with a glyph (or a letter) on it.
    void badge(const Rect& r, float radius, D2D1_COLOR_F hue, const std::wstring& mark, bool is_glyph, float size) {
        fill(r, radius, alpha(hue, pal.dark ? 0.2f : 0.13f));
        if (is_glyph) glyph(mark[0], r, size, hue);
        else text(mark, r, 0, size, kSemibold, hue, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    bool hot(Control c, int i = -1) const { return m.hot == c && (i < 0 || m.hot_index == i); }
    bool down(Control c, int i = -1) const { return m.pressed == c && hot(c, i) && (i < 0 || m.pressed_index == i); }
    float hover() const { return ease(m.hover_anim); }

    D2D1_COLOR_F code_fill() const { return pal.dark ? color(0x000000, 0.2f) : color(0x000000, 0.03f); }
    float char_width(float size) { return R.measure(L"0000000000", 3, size, kNormal) / 10.0f; }

    D2D1_COLOR_F tok_color(Tok t) const {
        switch (t) {
        case Tok::Keyword: return pal.code_keyword;
        case Tok::Type: return pal.code_type;
        case Tok::String: return pal.code_string;
        case Tok::Number: return pal.code_number;
        case Tok::Comment: return pal.code_comment;
        case Tok::Preproc: return pal.code_preproc;
        case Tok::Namespace: return pal.code_namespace;
        default: return pal.text;
        }
    }
    // One line of code in its colors; x is where the first character starts.
    void code(const std::string& s, const std::vector<Span>& spans, float x, float y, float h, float size, float cw, float right) {
        for (const Span& sp : spans) {
            const float sx = x + static_cast<float>(sp.begin) * cw;
            if (sx >= right) break;
            const float w = std::min(static_cast<float>(sp.end - sp.begin) * cw + cw * 2, right - sx);
            text(widen(s.substr(sp.begin, sp.end - sp.begin)), Rect{sx, y, w, h}, 3, size, kNormal, tok_color(sp.tok));
        }
    }

    void card(const Rect& r) {
        shadow(r, kCardRadius, 0.035f);
        fill(r, kCardRadius, pal.card);
        stroke(r, kCardRadius, pal.card_stroke);
    }
    void header(const Rect& card, const std::wstring& title, const std::wstring& sub) {
        text(title, Rect{card.x + 24, card.y + 20, card.w - 48, 30}, 1, 22, kSemibold, pal.text);
        text(sub, Rect{card.x + 24, card.y + 52, card.w - 48, 20}, 0, 14, kNormal, pal.text2);
    }
    // A panel that can be clicked: it brightens and gets an accent edge under the pointer.
    void tile(const Rect& r, bool is_hot, bool is_down, bool enabled, D2D1_COLOR_F edge) {
        const float h = enabled && is_hot ? hover() : 0.0f;
        if (h > 0) shadow(r, 8, 0.04f * h);
        const float a = enabled && is_down ? 0.03f : 0.045f + 0.035f * h;
        fill(r, 8, alpha(pal.text, pal.dark ? a : a * 0.75f));
        stroke(r, 8, pal.card_stroke);
        if (h > 0) stroke(r, 8, alpha(edge, 0.55f * h), 1.5f);
    }
    D2D1_COLOR_F gold() const { return pal.dark ? color(0xFFC83D) : color(0xC98A00); }

    // A button with a glyph before its words.
    void icon_button(Control c, const Rect& r, wchar_t icon, const std::wstring& label, bool enabled, bool accent) {
        button(c, r, L"", enabled, accent);
        const float tw = R.measure(label, 0, 15, accent ? kSemibold : kNormal);
        const float x = r.x + (r.w - tw - 26) / 2;
        const D2D1_COLOR_F fg = accent ? (enabled ? pal.accent_text : color(0xFFFFFF, 0.6f)) : alpha(pal.text, enabled ? 1.0f : 0.45f);
        glyph(icon, Rect{x, r.y, 16, r.h}, 13, fg);
        text(label, Rect{x + 26, r.y, tw + 4, r.h}, 0, 15, accent ? kSemibold : kNormal, fg);
    }

    void button(Control c, const Rect& r, const std::wstring& label, bool enabled, bool accent, int index = -1) {
        if (r.w <= 0) return;
        const bool h = hot(c, index), dn = down(c, index);
        if (accent) {
            D2D1_COLOR_F f = pal.accent;
            D2D1_COLOR_F t = pal.accent_text;
            if (!enabled) {
                f = pal.dark ? color(0xFFFFFF, 0.16f) : color(0x000000, 0.22f);
                t = pal.dark ? color(0xFFFFFF, 0.5f) : color(0xFFFFFF);
            } else if (dn) {
                f = alpha(f, 0.8f);
                t = alpha(t, 0.8f);
            } else if (h) {
                f = alpha(f, 1.0f - 0.1f * hover());
            }
            if (enabled) shadow(r, 6, 0.05f);
            fill(r, 6, f);
            if (enabled) hline(r.x + 6, r.right() - 6, r.bottom() - 1, color(0x000000, pal.dark ? 0.14f : 0.35f));
            text(label, r, 0, 15, kSemibold, t, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        const float dim = enabled ? 1.0f : 0.45f;
        const float hv = enabled && h ? hover() : 0.0f;
        D2D1_COLOR_F f;
        if (pal.dark) f = mix(color(0x2B2B2B), D2D1::ColorF(1, 1, 1), enabled && dn ? 0.06f : 0.08f + 0.03f * hv);
        else f = enabled && dn ? color(0xF5F5F5) : mix(color(0xFFFFFF), color(0xF6F6F6), hv);
        fill(r, 6, alpha(f, dim));
        stroke(r, 6, alpha(pal.stroke2, dim));
        hline(r.x + 6, r.right() - 6, r.bottom() - 1, alpha(pal.button_bottom, dim));
        const bool icon = label.size() == 1 && label[0] >= 0xE000;
        if (icon) glyph(label[0], r, 14, alpha(pal.text, enabled && dn ? 0.8f : dim));
        else text(label, r, 0, 15, kNormal, alpha(pal.text, enabled && dn ? 0.8f : dim), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void empty_state(const Rect& area, wchar_t g, D2D1_COLOR_F hue, const std::wstring& title, const std::wstring& sub) {
        const float mid = area.y + area.h / 2;
        badge(Rect{area.x + area.w / 2 - 36, mid - 98, 72, 72}, 36, hue, std::wstring(1, g), true, 30);
        text(title, Rect{area.x, mid - 12, area.w, 26}, 1, 18, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(sub, Rect{area.x + 32, mid + 18, area.w - 64, 44}, 0, 14, kNormal, pal.text2, DWRITE_TEXT_ALIGNMENT_CENTER, true,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    // Draws `body` at `opacity`, moved down by `dy`.
    template <class F>
    void faded(float opacity, float dy, F body) {
        if (opacity >= 0.999f && std::abs(dy) < 0.01f) {
            body();
            return;
        }
        ComPtr<ID2D1Layer> layer;
        if (FAILED(rt->CreateLayer(nullptr, &layer))) {
            body();
            return;
        }
        D2D1_MATRIX_3X2_F before;
        rt->GetTransform(&before);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                            std::clamp(opacity, 0.0f, 1.0f)),
                      layer.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * before);
        body();
        rt->SetTransform(before);
        rt->PopLayer();
    }
    // Draws `body` with its opacity at `t` (0..1), sliding up into place as it appears.
    template <class F>
    void appear(float t, float slide, F body) {
        const float e = ease(t);
        faded(e, (1 - e) * slide, body);
    }
};

D2D1_COLOR_F example_hue(const Palette& pal, std::size_t i) {
    switch (i % 3) {
    case 0: return pal.orange;
    case 1: return pal.purple;
    default: return pal.teal;
    }
}

D2D1_COLOR_F outcome_hue(const Palette& pal, Outcome o) {
    switch (o) {
    case Outcome::WontBuild: return pal.orange;
    case Outcome::Crashes: return pal.error;
    case Outcome::Different: return pal.purple;
    case Outcome::Same: return pal.teal;
    }
    return pal.accent;
}

const wchar_t* outcome_mark(Outcome o) {
    switch (o) {
    case Outcome::WontBuild: return L"?";
    case Outcome::Crashes: return L"!";
    case Outcome::Different: return L"≠";
    case Outcome::Same: return L"=";
    }
    return L"";
}

void draw_title_bar(Draw& d) {
    const Palette& pal = d.pal;
    const Layout& l = d.l;
    d.fill(l.app_icon, 4, pal.accent);
    d.text(L"{ }", l.app_icon, 3, 9, kSemibold, pal.accent_text, DWRITE_TEXT_ALIGNMENT_CENTER);
    std::wstring title = L"CppSparring";
    if (!d.m.source_name.empty()) title += L" — " + widen(d.m.source_name);
    d.text(title, l.title_text, 0, 12, kNormal, d.m.active ? pal.text : pal.text3);

    const struct {
        Control id;
        const Rect* rect;
        wchar_t glyph;
    } caps[] = {{Control::CaptionMinimize, &l.caption_minimize, L'\xE921'},
                {Control::CaptionMaximize, &l.caption_maximize, d.m.maximized ? L'\xE923' : L'\xE922'},
                {Control::CaptionClose, &l.caption_close, L'\xE8BB'}};
    for (const auto& c : caps) {
        D2D1_COLOR_F g = d.m.active ? pal.text : pal.text3;
        if (d.m.hot == c.id) {
            if (c.id == Control::CaptionClose) {
                d.fill(*c.rect, 0, d.m.pressed == c.id ? color(0xC42B1C, 0.9f) : color(0xC42B1C));
                g = D2D1::ColorF(1, 1, 1);
            } else {
                d.fill(*c.rect, 0, alpha(pal.dark ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0), d.m.pressed == c.id ? 0.04f : 0.06f));
            }
        }
        d.glyph(c.glyph, *c.rect, 10, g);
    }
}

void draw_rail(Draw& d) {
    const Palette& pal = d.pal;
    const bool loaded = !d.m.source_path.empty();
    const struct {
        Control id;
        Page page;
        const Rect* rect;
        wchar_t glyph;
        const wchar_t* label;
        bool enabled;
    } items[] = {{Control::NavHome, Page::Home, &d.l.nav_home, L'\xE80F', L"Home", true},
                 {Control::NavPractice, Page::Practice, &d.l.nav_practice, L'\xE945', L"Practice", true},
                 {Control::NavLearn, Page::Lessons, &d.l.nav_learn, L'\xE7BE', L"Lessons", true},
                 {Control::NavBadges, Page::Badges, &d.l.nav_badges, L'\xE734', L"Badges", true}};
    (void)loaded;
    if (d.l.rail.w <= 0) return;
    for (const auto& it : items) {
        const Rect& r = *it.rect;
        const Page p = d.m.page;
        const bool on = p == it.page || (it.page == Page::Lessons && p == Page::Lesson) ||
                        (it.page == Page::Practice && (p == Page::Look || p == Page::Break)) ||
                        (it.page == Page::Home && (p == Page::Quiz || p == Page::QuestDone));
        const float dim = it.enabled ? 1.0f : 0.4f;
        if (on) d.fill(r, 6, alpha(pal.text, pal.dark ? 0.07f : 0.055f));
        else if (it.enabled && d.hot(it.id)) d.fill(r, 6, alpha(pal.text, (pal.dark ? 0.05f : 0.035f) * d.hover()));
        if (on) d.fill(Rect{r.x, r.y + 18, 3, 20}, 1.5f, pal.accent);
        d.glyph(it.glyph, Rect{r.x, r.y + 8, r.w, 22}, 18, on ? pal.accent : alpha(pal.text, dim));
        d.text(it.label, Rect{r.x - 4, r.y + 32, r.w + 8, 16}, 0, 10, on ? kSemibold : kNormal, alpha(on ? pal.text : pal.text2, dim),
               DWRITE_TEXT_ALIGNMENT_CENTER);
        if (it.id == Control::NavBadges && !d.m.progress.badges.empty()) {
            const std::wstring n = std::to_wstring(d.m.progress.badges.size());
            const Rect dot{r.right() - 20, r.y + 4, 16, 16};
            d.circle(dot, d.gold());
            d.text(n, dot, 1, 9, kSemibold, color(0x000000), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void draw_status(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    if (d.l.status_bar.h <= 0) return;
    d.hline(0, d.l.width, d.l.status_bar.y, pal.stroke2);
    D2D1_COLOR_F dot = pal.ok;
    std::string text;
    switch (m.page) {
    case Page::Home: text = "Press the big button to start. Or open a door below."; break;
    case Page::Practice: text = "Pick a program to play with."; break;
    case Page::Look:
        text = m.replay_open ? "Press play, or step through one line at a time with the arrow buttons."
                             : "Read what's inside. Click one on the right to find it in the code.";
        break;
    case Page::Break: text = "Guess what will happen, then see if you were right."; break;
    case Page::Badges: text = "Your badges and stars stay, even after you close CppSparring."; break;
    case Page::Lessons: text = "Pick a lesson. You'll write a little C, and the computer checks it for real."; break;
    case Page::Lesson: text = "Write the code, then press Run (or F5). Stuck? A hint is one click away."; break;
    case Page::Quiz: text = "Answer from memory. Remembering is what makes it stick."; break;
    case Page::QuestDone: text = "Quest done! Open the chest, then go again or take a break."; break;
    }
    if (m.importing) {
        dot = pal.accent;
        text = "Reading your program…";
    } else if (m.running) {
        dot = pal.accent;
        text = "Building and running both versions for real…";
    } else if (!m.import_error.empty() && m.nodes.empty()) {
        dot = pal.error;
        text = m.import_error;
    }
    d.circle(d.l.status_dot, dot);
    d.text(widen(text), d.l.status_text, 0, 12, kNormal, pal.text2);
    // pills from the right: stars, the streak, this program's score
    float right = d.l.status_right.right();
    auto pill = [&](const std::wstring& s, wchar_t g, D2D1_COLOR_F hue) {
        const float w = d.R.measure(s, 1, 12, kSemibold) + 40;
        const Rect p{right - w, d.l.status_bar.y + 5, w, 22};
        d.fill(p, 11, alpha(hue, pal.dark ? 0.2f : 0.13f));
        d.glyph(g, Rect{p.x + 8, p.y, 16, p.h}, 11, hue);
        d.text(s, Rect{p.x + 28, p.y, w - 34, p.h}, 1, 12, kSemibold, pal.text);
        right = p.x - 8;
    };
    pill(std::format(L"{} stars", total_stars(m.progress)), L'\xE735', d.gold());
    pill(std::format(L"{} XP", m.progress.xp), L'\xE7C1', pal.purple);
    if (m.progress.streak >= 2) pill(std::format(L"{} in a row", m.progress.streak), L'\xE945', pal.accent);
}

void draw_stars(Draw& d, float x, float y, int stars, float size) {
    for (int i = 0; i < 3; ++i)
        d.glyph(i < stars ? L'\xE735' : L'\xE734', Rect{x + static_cast<float>(i) * (size + 4), y, size, size}, size - 2,
                i < stars ? d.gold() : d.pal.text3);
}

// ---- step 1: pick a program
void draw_hero(Draw& d, const Rect& r) {
    const Palette& pal = d.pal;
    ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP gs[2] = {{0.0f, pal.hero_a}, {1.0f, pal.hero_b}};
    ComPtr<ID2D1LinearGradientBrush> gradient;
    d.shadow(r, 10, 0.06f);
    if (SUCCEEDED(d.rt->CreateGradientStopCollection(gs, 2, &stops)) &&
        SUCCEEDED(d.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.right(), r.bottom())),
                                                  stops.Get(), &gradient)))
        d.rt->FillRoundedRectangle(D2D1::RoundedRect(rf(r), 10, 10), gradient.Get());
    else d.fill(r, 10, pal.hero_a);

    const Model& m = d.m;
    const Progress& p = m.progress;
    const D2D1_COLOR_F white = color(0xFFFFFF);

    // ---- the right side: rank and XP, and this week's days
    const Rect& side = d.l.hero_side;
    d.fill(side, 12, color(0x000000, 0.18f));
    const std::size_t ri = rank_index(p.xp);
    const Rank& rank = ranks()[ri];
    const bool top_rank = ri + 1 >= ranks().size();
    d.glyph(L'\xE7C1', Rect{side.x + 14, side.y + 10, 20, 22}, 14, d.gold());
    d.text(widen(rank.name), Rect{side.x + 40, side.y + 10, side.w - 140, 22}, 1, 15, kSemibold, white);
    d.text(std::format(L"{} XP", p.xp), Rect{side.right() - 110, side.y + 10, 96, 22}, 1, 13, kSemibold, color(0xFFFFFF, 0.85f),
           DWRITE_TEXT_ALIGNMENT_TRAILING);
    const Rect bar{side.x + 14, side.y + 38, side.w - 28, 8};
    const float t = top_rank ? 1.0f
                             : static_cast<float>(p.xp - rank.xp) / static_cast<float>(std::max(1, ranks()[ri + 1].xp - rank.xp));
    d.fill(bar, 4, color(0xFFFFFF, 0.2f));
    d.fill(Rect{bar.x, bar.y, bar.w * std::clamp(t, 0.0f, 1.0f), bar.h}, 4, d.gold());
    if (!top_rank)
        d.text(widen(std::to_string(ranks()[ri + 1].xp - p.xp) + " XP to " + ranks()[ri + 1].name), Rect{side.x + 14, side.y + 48, side.w - 28, 16}, 0, 11,
               kNormal, color(0xFFFFFF, 0.75f));
    // the week: one dot per day, filled when played; the goal is a few days, not every day
    const std::vector<std::string> week = this_week();
    const wchar_t* names[] = {L"M", L"T", L"W", L"T", L"F", L"S", L"S"};
    const std::string now = today();
    const float dot_y = side.y + 74;
    for (int i = 0; i < 7; ++i) {
        const Rect dot{side.x + 14 + static_cast<float>(i) * 26, dot_y, 20, 20};
        const bool played = p.days.count(week[static_cast<std::size_t>(i)]) > 0;
        d.circle(dot, played ? d.gold() : color(0xFFFFFF, 0.16f));
        if (week[static_cast<std::size_t>(i)] == now) d.stroke(Rect{dot.x - 3, dot.y - 3, 26, 26}, 13, white, 1.5f);
        d.text(names[i], dot, 0, 10, kSemibold, played ? color(0x000000) : color(0xFFFFFF, 0.8f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const int played = days_this_week(p);
    d.text(played >= kWeekGoal ? std::wstring(L"Week goal done!") : std::format(L"{} of {} days", played, kWeekGoal),
           Rect{side.x + 14 + 7 * 26, dot_y, side.w - 28 - 7 * 26, 20}, 0, 11, kSemibold, color(0xFFFFFF, 0.9f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    d.text(std::format(L"{} quest{} done   ·   best streak {}", p.quests_done, p.quests_done == 1 ? L"" : L"s", p.best_streak),
           Rect{side.x + 14, side.bottom() - 32, side.w - 28, 20}, 0, 11, kNormal, color(0xFFFFFF, 0.75f));

    // ---- the left side: what this is, and one big button to start
    const float text_w = side.x - 40 - (r.x + 44);
    const float mid = r.y + r.h / 2;
    const int until = quests_until_boss(p);
    const bool resume = m.quest.active && !m.quest.finished;
    d.text(resume ? L"Welcome back!" : until == 0 ? L"A Bug Boss is waiting" : L"Ready to break some code?", Rect{r.x + 44, mid - 92, text_w, 48}, 1,
           36, kSemibold, white);
    const std::wstring sub = resume       ? L"You're in the middle of a quest. Jump back in where you left off."
                             : until == 0 ? L"Five quick guesses. Get three right to knock it out."
                                          : std::format(L"Five quick steps, about 5 minutes. A Bug Boss shows up in {} quest{}.", until, until == 1 ? L"" : L"s");
    d.text(sub, Rect{r.x + 44, mid - 40, text_w, 44}, 0, 16, kNormal, color(0xFFFFFF, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING, true,
           DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const Rect& b = d.l.hero_button;
    const float h = d.hot(Control::HeroQuest) ? d.hover() : 0.0f;
    d.shadow(b, 32, 0.1f + 0.08f * h);
    d.fill(b, 32, mix(white, color(0xF0F0F0), d.down(Control::HeroQuest) ? 1.0f : h * 0.6f));
    const std::wstring label = resume ? L"Continue quest" : until == 0 ? L"Fight the Bug Boss" : L"Start today's quest";
    const float lw = d.R.measure(label, 1, 20, kSemibold);
    const float x0 = b.x + (b.w - lw - 34) / 2;
    d.glyph(until == 0 && !resume ? L'\xEBE8' : L'\xE768', Rect{x0, b.y, 22, b.h}, 17, pal.hero_a);
    d.text(label, Rect{x0 + 34, b.y, lw + 4, b.h}, 1, 20, kSemibold, pal.hero_a);
}

// Home: the one big button, and three doors under it.
void draw_home(Draw& d) {
    const Palette& pal = d.pal;
    const Layout& l = d.l;
    const Progress& p = d.m.progress;
    draw_hero(d, l.hero);
    int done = 0;
    for (const Lesson& ls : lessons()) done += p.lessons_done.count(ls.id) > 0;
    int next = -1;
    for (std::size_t i = 0; i < lessons().size() && next < 0; ++i)
        if (!p.lessons_done.count(lessons()[i].id)) next = static_cast<int>(i);
    const int level = highest_open_level(p);
    const struct {
        Control id;
        wchar_t g;
        D2D1_COLOR_F hue;
        const wchar_t* title;
        std::wstring line1, line2;
    } doors[] = {
        {Control::DoorPractice, L'\xE945', pal.orange, L"Practice",
         std::format(L"Level {}: {}", level, widen(levels()[static_cast<std::size_t>(level - 1)].name)),
         std::format(L"{} of {} stars", total_stars(p), examples().size() * 3)},
        {Control::DoorLessons, L'\xE7BE', pal.teal, L"Lessons", std::format(L"{} of {} done", done, lessons().size()),
         next >= 0 ? widen("Up next: " + lessons()[static_cast<std::size_t>(next)].title) : std::wstring(L"All done!")},
        {Control::DoorBadges, L'\xE734', pal.purple, L"Badges", std::format(L"{} of {} earned", p.badges.size(), all_badges().size()),
         widen(std::string("Rank: ") + ranks()[rank_index(p.xp)].name)},
    };
    for (std::size_t i = 0; i < 3; ++i) {
        const Rect& r = l.doors[i];
        const auto& door = doors[i];
        d.tile(r, d.hot(door.id), d.down(door.id), true, door.hue);
        d.shadow(r, 8, 0.02f);
        const float mid = r.y + r.h / 2;
        d.badge(Rect{r.x + 22, mid - 28, 56, 56}, 16, door.hue, std::wstring(1, door.g), true, 24);
        d.text(door.title, Rect{r.x + 94, mid - 34, r.w - 130, 28}, 1, 20, kSemibold, pal.text);
        d.text(door.line1, Rect{r.x + 94, mid - 4, r.w - 130, 20}, 0, 13, kSemibold, door.hue);
        d.text(door.line2, Rect{r.x + 94, mid + 16, r.w - 130, 20}, 0, 13, kNormal, pal.text2);
        d.glyph(L'\xE76C', Rect{r.right() - 40, mid - 12, 24, 24}, 14, alpha(pal.text, 0.5f + 0.5f * (d.hot(door.id) ? d.hover() : 0.0f)));
    }
}

// Inside a program or a lesson: back to where it came from, and a program's two steps.
void draw_session_bar(Draw& d) {
    const Layout& l = d.l;
    if (l.session_bar.w <= 0) return;
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Rect& r = l.session_bar;
    d.fill(r, 10, alpha(pal.text, pal.dark ? 0.04f : 0.03f));
    d.stroke(r, 10, pal.card_stroke);
    const bool lesson = m.page == Page::Lesson;
    d.icon_button(Control::SessionBack, l.session_back, L'\xE72B', L"Back", true, false);
    std::wstring where;
    if (lesson && m.lesson >= 0 && m.lesson < static_cast<int>(lessons().size()))
        where = std::format(L"Lessons  ›  Lesson {} of {}", m.lesson + 1, lessons().size());
    else
        where = L"Practice  ›  " + widen(m.source_name);
    d.text(where, Rect{l.session_back.right() + 14, r.y, r.w - 420, r.h}, 0, 14, kSemibold, pal.text2);
    if (lesson) return;
    const struct {
        Control id;
        const Rect* rect;
        const wchar_t* label;
        Page page;
    } steps[] = {{Control::StepLook, &l.step_look, L"1  Look inside", Page::Look}, {Control::StepBreak, &l.step_break, L"2  Break it", Page::Break}};
    for (const auto& s : steps) {
        const bool on = m.page == s.page;
        const Rect& sr = *s.rect;
        if (on) d.fill(sr, 16, pal.accent);
        else d.tile(sr, d.hot(s.id), d.down(s.id), true, pal.accent);
        d.text(s.label, sr, 0, 13, kSemibold, on ? pal.accent_text : pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    d.text(L"›", Rect{l.step_look.right(), r.y, 8, r.h}, 0, 14, kSemibold, pal.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// The quest's progress over the page: one segment per step, and a way to skip or stop without any penalty.
void draw_quest_bar(Draw& d) {
    const Model& m = d.m;
    const Layout& l = d.l;
    if (l.quest_bar.w <= 0) return;
    const Palette& pal = d.pal;
    const Quest& q = m.quest;
    const Rect& r = l.quest_bar;
    d.fill(r, 10, q.boss ? alpha(pal.error, pal.dark ? 0.16f : 0.1f) : alpha(pal.accent, pal.dark ? 0.14f : 0.09f));
    d.stroke(r, 10, q.boss ? alpha(pal.error, 0.4f) : alpha(pal.accent, 0.35f));
    const std::wstring title = q.boss ? L"BUG BOSS" : L"DAILY QUEST";
    d.glyph(q.boss ? L'\xEBE8' : L'\xE7C1', Rect{r.x + 12, r.y, 20, r.h}, 14, q.boss ? pal.error : pal.accent);
    d.text(title, Rect{r.x + 38, r.y, 120, r.h}, 1, 13, kSemibold, q.boss ? pal.error : pal.accent);
    const float seg_x = r.x + 160;
    const float seg_w = std::max(0.0f, l.quest_skip.x - 140 - seg_x);
    const float each = q.steps.empty() ? 0 : (seg_w - 6.0f * static_cast<float>(q.steps.size() - 1)) / static_cast<float>(q.steps.size());
    for (std::size_t i = 0; i < q.steps.size(); ++i) {
        const QuestStep& s = q.steps[i];
        const Rect seg{seg_x + static_cast<float>(i) * (each + 6), r.y + r.h / 2 - 5, each, 10};
        D2D1_COLOR_F c = alpha(pal.text, 0.14f);
        if (s.done) c = s.skipped ? alpha(pal.text, 0.3f) : s.right ? pal.ok : pal.warn;
        else if (static_cast<int>(i) == q.at) c = q.boss ? pal.error : pal.accent;
        d.fill(seg, 5, c);
    }
    d.text(std::format(L"Step {} of {}   +{} XP", std::min(q.at + 1, static_cast<int>(q.steps.size())), q.steps.size(), q.xp),
           Rect{l.quest_skip.x - 136, r.y, 128, r.h}, 0, 12, kSemibold, pal.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
    d.button(Control::QuestSkip, l.quest_skip, L"Skip", true, false);
    d.button(Control::QuestQuit, l.quest_quit, std::wstring(1, L'\xE711'), true, false);
}

// Practice: the programs by level, and a way to open your own.
void draw_practice(Draw& d) {
    const Palette& pal = d.pal;
    const Layout& l = d.l;
    d.card(l.card_a);
    d.header(l.card_a, L"Practice", L"Pick a program. You'll look inside it first, then try to break it.");
    // ---- the levels
    const Progress& pr = d.m.progress;
    for (std::size_t i = 0; i < levels().size() && i < l.level_tabs.size(); ++i) {
        const Level& lv = levels()[i];
        const Rect& r = l.level_tabs[i];
        const bool open = level_open(pr, lv.number);
        const bool on = d.m.pick_level == lv.number;
        const bool is_hot = d.hot(Control::LevelTab, static_cast<int>(i));
        if (on) {
            d.fill(r, 8, alpha(pal.accent, pal.dark ? 0.2f : 0.12f));
            d.stroke(r, 8, alpha(pal.accent, 0.7f), 1.5f);
        } else {
            d.tile(r, is_hot, d.down(Control::LevelTab, static_cast<int>(i)), true, pal.accent);
        }
        const float dim = open ? 1.0f : 0.5f;
        d.text(std::format(L"LEVEL {}", lv.number), Rect{r.x + 14, r.y + 4, r.w - 60, 14}, 0, 10, kSemibold, alpha(on ? pal.accent : pal.text3, dim));
        d.text(widen(lv.name), Rect{r.x + 14, r.y + 18, r.w - 60, 18}, 1, 15, kSemibold, alpha(pal.text, dim));
        if (!open) {
            d.glyph(L'\xE72E', Rect{r.right() - 34, r.y, 22, r.h}, 13, pal.text3);
        } else {
            int got = 0, most = 0;
            for (std::size_t k : examples_in_level(lv.number)) {
                got += stars_for(pr, examples()[k].file);
                most += 3;
            }
            d.glyph(L'\xE735', Rect{r.right() - 58, r.y, 16, r.h}, 11, got > 0 ? d.gold() : pal.text3);
            d.text(std::format(L"{}/{}", got, most), Rect{r.right() - 40, r.y, 32, r.h}, 1, 11, kSemibold, pal.text2);
        }
    }
    const Level& level = levels()[static_cast<std::size_t>(std::clamp(d.m.pick_level, 1, 4) - 1)];
    const std::wstring lname = std::format(L"Level {}: {}", level.number, widen(level.name));
    const float lw = d.R.measure(lname, 1, 15, kSemibold);
    d.text(lname, Rect{l.examples_label.x, l.examples_label.y, lw + 4, l.examples_label.h}, 1, 15, kSemibold, pal.text);
    d.text(widen(level.teaches), Rect{l.examples_label.x + lw + 12, l.examples_label.y, l.examples_label.w - lw - 12, l.examples_label.h}, 0, 13, kNormal,
           pal.text2);

    if (!level_open(pr, level.number)) {
        const int need = std::max(0, kRightToOpenNextLevel - right_in_level(pr, level.number - 1));
        d.fill(l.tiles_area, 8, alpha(pal.text, 0.03f));
        d.stroke(l.tiles_area, 8, pal.card_stroke, 1.0f, d.R.dash.Get());
        d.empty_state(Rect{l.tiles_area.x, l.tiles_area.y + 20, l.tiles_area.w, l.tiles_area.h}, L'\xE72E', d.gold(),
                      std::format(L"Level {} is locked", level.number),
                      std::format(L"Guess right {} more time{} in Level {} to open it.", need, need == 1 ? L"" : L"s", level.number - 1));
    }

    const auto& ex = examples();
    for (std::size_t t = 0; t < l.examples.size() && level_open(pr, level.number); ++t) {
        const std::size_t i = l.example_items[t];
        const Rect& r = l.examples[t];
        const D2D1_COLOR_F hue = example_hue(pal, t);
        const bool current = d.m.source_name == ex[i].file;
        d.tile(r, d.hot(Control::Example, static_cast<int>(t)), d.down(Control::Example, static_cast<int>(t)), true, hue);
        if (current) d.stroke(r, 8, hue, 2.0f);
        d.badge(Rect{r.x + 20, r.y + 20, 48, 48}, 12, hue, std::wstring(1, ex[i].glyph), true, 22);
        draw_stars(d, r.right() - 20 - 3 * 18 + 4, r.y + 22, stars_for(pr, ex[i].file), 16);
        d.text(widen(ex[i].title), Rect{r.x + 20, r.y + 80, r.w - 40, 24}, 1, 18, kSemibold, pal.text);
        d.para(ex[i].blurb, Rect{r.x + 20, r.y + 106, r.w - 40, 36}, 13, pal.text2);
        // a peek at the code, in its colors
        const Rect peek{r.x + 16, r.y + 148, r.w - 32, r.bottom() - 40 - (r.y + 148)};
        if (peek.h >= 40) {
            d.fill(peek, 6, d.code_fill());
            const std::vector<std::string> lines = split_lines(ex[i].source);
            std::size_t first = 0;
            while (first < lines.size() && (lines[first].empty() || lines[first][0] == '#')) ++first;
            const float cw = d.char_width(11.5f);
            bool in_block = false;
            d.rt->PushAxisAlignedClip(rf(peek), D2D1_ANTIALIAS_MODE_ALIASED);
            float y = peek.y + 8;
            for (std::size_t k = first; k < lines.size() && y + 17 <= peek.bottom() - 4; ++k) {
                d.code(lines[k], lex_line(lines[k], in_block), peek.x + 12, y, 17, 11.5f, cw, peek.right() - 8);
                y += 17;
            }
            d.rt->PopAxisAlignedClip();
        }
        d.text(current ? L"Playing now" : L"Open it  ›", Rect{r.x + 20, r.bottom() - 32, r.w - 40, 20}, 0, 13, kSemibold, hue);
    }
    const Rect& dz = l.drop_area;
    d.stroke(dz, kCardRadius, pal.dash, 1.0f, d.R.dash.Get());
    const float mid = dz.y + dz.h / 2;
    d.badge(Rect{dz.x + 24, mid - 22, 44, 44}, 22, pal.accent, std::wstring(1, L'\xE898'), true, 18);
    d.text(L"Have your own program?", Rect{dz.x + 84, mid - 22, dz.w - 300, 22}, 0, 15, kSemibold, pal.text);
    d.text(L"Drag a .cpp file into this box, or open one from your computer.", Rect{dz.x + 84, mid, dz.w - 300, 20}, 0, 13, kNormal, pal.text2);
    d.button(Control::OpenFile, l.open_button, L"Open my own file", true, false);
}

// ---- step 2: look inside
void draw_code(Draw& d, const Rect& cv, const std::string& source) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    d.fill(cv, 6, d.code_fill());
    d.stroke(cv, 6, pal.card_stroke);
    const std::vector<std::string> lines = split_lines(source);
    const int visible = std::max(1, static_cast<int>((cv.h - 16) / kCodeLineHeight));
    const int first = std::clamp(m.code_scroll, 0, std::max(0, static_cast<int>(lines.size()) - visible));
    const float gutter = 48;
    const float cw = d.char_width(13);
    bool in_block = false;
    for (int i = 0; i < first; ++i) lex_line(lines[static_cast<std::size_t>(i)], in_block);
    d.rt->PushAxisAlignedClip(rf(cv), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < visible && first + i < static_cast<int>(lines.size()); ++i) {
        const std::string& s = lines[static_cast<std::size_t>(first + i)];
        const float y = cv.y + 8 + static_cast<float>(i) * kCodeLineHeight;
        const bool lit = first + i + 1 == m.highlight_line;
        if (lit) {
            d.fill(Rect{cv.x + 1, y, cv.w - 2, kCodeLineHeight}, 0, alpha(pal.accent, 0.16f));
            d.fill(Rect{cv.x + 1, y, 3, kCodeLineHeight}, 0, pal.accent);
        }
        d.text(std::to_wstring(first + i + 1), Rect{cv.x, y, gutter - 14, kCodeLineHeight}, 3, 13, kNormal, lit ? pal.accent : pal.text3,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
        d.code(s, lex_line(s, in_block), cv.x + gutter, y, kCodeLineHeight, 13, cw, cv.right() - 8);
    }
    d.rt->PopAxisAlignedClip();
}

// The boxes as they are at this step: one small card each, lit when it just changed.
void draw_boxes(Draw& d, const Rect& area, const ReplayStep& step, const ReplayStep* prev) {
    const Palette& pal = d.pal;
    d.text(L"BOXES RIGHT NOW", Rect{area.x, area.y, area.w, 18}, 0, 11, kSemibold, pal.text3);
    if (step.vars.empty()) {
        d.text(L"No boxes here yet.", Rect{area.x, area.y + 26, area.w, 20}, 0, 13, kNormal, pal.text2);
        return;
    }
    const float cw = (area.w - 12) / 2, ch = 62;
    const int rows = std::max(1, static_cast<int>((area.h - 26 + 10) / (ch + 10)));
    const float glow = 1.0f - ease(d.m.step_anim);
    for (std::size_t i = 0; i < step.vars.size(); ++i) {
        const int row = static_cast<int>(i / 2);
        if (row >= rows) {
            d.text(std::format(L"+ {} more", step.vars.size() - i), Rect{area.x, area.bottom() - 16, area.w, 16}, 0, 11, kNormal, pal.text3,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
            break;
        }
        const ReplayVar& v = step.vars[i];
        const ReplayVar* was = nullptr;
        if (prev)
            for (const ReplayVar& p : prev->vars)
                if (p.name == v.name) was = &p;
        const bool fresh = prev && !was;
        const bool changed = was && was->value != v.value;
        const Rect r{area.x + static_cast<float>(i % 2) * (cw + 12), area.y + 26 + static_cast<float>(row) * (ch + 10), cw, ch};
        const D2D1_COLOR_F hue = changed ? pal.purple : fresh ? pal.ok : pal.accent;
        d.fill(r, 8, changed || fresh ? alpha(hue, (pal.dark ? 0.16f : 0.1f) + 0.14f * glow) : alpha(pal.text, pal.dark ? 0.05f : 0.035f));
        d.stroke(r, 8, changed || fresh ? alpha(hue, 0.6f + 0.4f * glow) : pal.card_stroke, changed || fresh ? 1.5f : 1.0f);
        d.text(widen(v.name), Rect{r.x + 12, r.y + 6, r.w - 24, 18}, 3, 12, kNormal, pal.text2);
        d.text(widen(v.value), Rect{r.x + 12, r.y + 24, r.w - 24, 30}, 1, 20, kSemibold, pal.text);
        if (changed) d.text(widen("was " + was->value), Rect{r.x + 12, r.y + 6, r.w - 24, 18}, 0, 11, kSemibold, pal.purple, DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (fresh) d.text(L"new!", Rect{r.x + 12, r.y + 6, r.w - 24, 18}, 0, 11, kSemibold, pal.ok, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void draw_console(Draw& d, const Rect& r, const std::string& printed, std::size_t new_from, const std::wstring& note,
                  const wchar_t* title = L"Printed so far") {
    const Palette& pal = d.pal;
    d.fill(r, 6, d.code_fill());
    d.stroke(r, 6, pal.card_stroke);
    const Rect head{r.x + 1, r.y + 1, r.w - 2, 28};
    d.fill(head, 5, alpha(pal.text, pal.dark ? 0.05f : 0.035f));
    d.hline(r.x + 1, r.right() - 1, head.bottom(), pal.card_stroke);
    d.glyph(L'\xE756', Rect{head.x + 8, head.y, 18, head.h}, 12, pal.text3);
    d.text(title, Rect{head.x + 30, head.y, head.w - 40, head.h}, 0, 12, kSemibold, pal.text2);
    // the last lines that fit, the newest ones lit
    std::vector<std::pair<std::string, bool>> lines;
    std::string cur;
    bool cur_new = false;
    for (std::size_t i = 0; i < printed.size(); ++i) {
        if (i >= new_from) cur_new = true;
        if (printed[i] == '\n') {
            lines.push_back({cur, cur_new});
            cur.clear();
            cur_new = false;
        } else if (printed[i] != '\r') {
            cur += printed[i];
        }
    }
    if (!cur.empty()) lines.push_back({cur, cur_new});
    const float lh = 19;
    const int fit = std::max(1, static_cast<int>((r.bottom() - head.bottom() - 12 - (note.empty() ? 0 : lh)) / lh));
    const std::size_t first = lines.size() > static_cast<std::size_t>(fit) ? lines.size() - static_cast<std::size_t>(fit) : 0;
    float y = head.bottom() + 6;
    if (lines.empty()) {
        d.text(L"(nothing yet)", Rect{r.x + 12, y, r.w - 24, lh}, 0, 12, kNormal, pal.text3);
        y += lh;
    }
    for (std::size_t i = first; i < lines.size(); ++i) {
        if (lines[i].second) {
            d.fill(Rect{r.x + 1, y, r.w - 2, lh}, 0, alpha(pal.ok, pal.dark ? 0.18f : 0.12f));
            d.fill(Rect{r.x + 1, y, 3, lh}, 0, pal.ok);
        }
        d.text(widen(lines[i].first), Rect{r.x + 12, y, r.w - 24, lh}, 3, 12.5f, kNormal, pal.text);
        y += lh;
    }
    if (!note.empty()) d.text(note, Rect{r.x + 12, y, r.w - 24, lh}, 0, 12, kSemibold, pal.error);
}

void draw_replay(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    const Rect& b = l.card_b;
    const Replay& rp = m.replay;
    const bool ready = !m.replay_loading && rp.ok && !rp.steps.empty();
    const std::size_t at = ready ? static_cast<std::size_t>(std::clamp(m.replay_step, 0, static_cast<int>(rp.steps.size()) - 1)) : 0;
    std::wstring where = L"Getting ready…";
    if (ready) {
        const std::string& fn = rp.steps[at].function;
        where = fn == "main" ? L"Now in main, the starting point" : widen("Now inside the job " + fn);
    }
    d.header(b, widen(m.replay_title), where);
    const Rect body{b.x, b.y + 80, b.w, l.replay_stop.y - 80 - b.y};
    if (m.replay_loading) {
        d.empty_state(body, L'\xE895', pal.accent, L"Getting ready…", L"We're building the program with little notes that report each step.");
    } else if (!rp.ok) {
        d.empty_state(body, L'\xE7BA', pal.orange, L"This one can't be watched yet", widen(rp.error.empty() ? "It didn't build." : rp.error));
    } else if (rp.steps.empty()) {
        d.empty_state(body, L'\xE897', pal.orange, L"Nothing to watch", L"The program finished before any line we could report on.");
    } else {
        const int n = static_cast<int>(rp.steps.size());
        const bool last = static_cast<int>(at) + 1 >= n;
        d.button(Control::ReplayBack, l.replay_back, std::wstring(1, L'\xE892'), at > 0, false);
        d.button(Control::ReplayPlay, l.replay_play, L"", true, true);
        d.glyph(m.replay_playing ? L'\xE769' : L'\xE768', l.replay_play, 16, pal.accent_text);
        d.button(Control::ReplayNext, l.replay_next, std::wstring(1, L'\xE893'), !last, false);
        d.text(std::format(L"Step {} of {}{}", at + 1, n, rp.cut_short ? L"+" : L""), Rect{l.replay_count.x, l.replay_count.y + 2, l.replay_count.w, 22},
               1, 17, kSemibold, pal.text);
        d.text(std::format(L"line {}", rp.steps[at].line), Rect{l.replay_count.x, l.replay_count.y + 24, l.replay_count.w, 18}, 0, 12, kNormal, pal.text2);
        // the bar: how far along, with a knob to drag
        const Rect& bar = l.replay_bar;
        const float t = n > 1 ? static_cast<float>(at) / static_cast<float>(n - 1) : 1.0f;
        d.fill(Rect{bar.x, bar.y + 4, bar.w, 6}, 3, alpha(pal.text, 0.12f));
        d.fill(Rect{bar.x, bar.y + 4, bar.w * t, 6}, 3, pal.accent);
        const float kx = bar.x + bar.w * t;
        d.circle(Rect{kx - 7, bar.y, 14, 14}, pal.accent);
        d.circle(Rect{kx - 3, bar.y + 4, 6, 6}, pal.dark ? color(0x000000, 0.4f) : color(0xFFFFFF));

        const ReplayStep* prev = at > 0 ? &rp.steps[at - 1] : nullptr;
        draw_boxes(d, l.replay_vars, rp.steps[at], prev);
        std::wstring note;
        if (last && rp.timed_out) note = L"(got stuck here; stopped after 5 seconds)";
        else if (last && rp.crashed) note = L"(crashed after this line)";
        const std::string printed = last ? rp.output : rp.steps[at].printed;
        draw_console(d, l.replay_console, printed, prev ? prev->printed.size() : 0, note);
    }
    d.button(Control::ReplayStop, l.replay_stop, L"Stop watching", true, false);
}

void draw_look(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    d.card(l.card_a);
    if (m.replay_open) {
        d.header(l.card_a, widen(m.source_name), m.replay_from == Page::Break ? L"With your change. The lit line is where the program is now."
                                                                              : L"The lit line is where the program is now.");
        draw_code(d, l.code_view, m.replay.source.empty() ? m.source_code : m.replay.source);
        d.card(l.card_b);
        draw_replay(d);
        return;
    }
    d.header(l.card_a, widen(m.source_name), L"This is the whole program, line by line.");
    draw_code(d, l.code_view, m.source_code);

    d.card(l.card_b);
    d.header(l.card_b, L"What's inside", L"Click one to find it in the code.");
    if (m.importing) {
        d.empty_state(l.look_list, L'\xE895', pal.accent, L"Reading your program…", L"This only takes a moment.");
    } else if (m.look.empty()) {
        d.empty_state(l.look_list, L'\xE897', pal.orange, L"Nothing to explain",
                      widen(m.import_error.empty() ? "This file has no jobs or boxes in it." : m.import_error));
    }
    for (std::size_t i = 0; i < l.rows.size(); ++i) {
        const Rect& r = l.rows[i];
        const LookItem& it = m.look[static_cast<std::size_t>(l.look_first) + i];
        const bool lit = it.line == m.highlight_line;
        wchar_t g = L'\xE713';
        const wchar_t* word = L"Job (a function)";
        D2D1_COLOR_F hue = pal.accent;
        switch (it.kind) {
        case LookKind::Start: g = L'\xE768'; word = L"Starting point"; hue = pal.ok; break;
        case LookKind::Job: break;
        case LookKind::Blueprint: g = L'\xE8A5'; word = L"Blueprint (a struct)"; hue = pal.purple; break;
        case LookKind::Box: g = L'\xE7B8'; word = L"Box (a variable)"; hue = pal.orange; break;
        }
        if (lit) {
            d.fill(r, 8, alpha(hue, pal.dark ? 0.14f : 0.09f));
            d.fill(Rect{r.x, r.y + 14, 3, r.h - 28}, 1.5f, hue);
        } else if (d.hot(Control::LookRow, static_cast<int>(i))) {
            d.fill(r, 8, alpha(pal.subtle, d.hover()));
        }
        d.badge(Rect{r.x + 14, r.y + 14, 38, 38}, 10, hue, std::wstring(1, g), true, 17);
        const std::wstring name = widen(it.name);
        const float nw = d.R.measure(name, 3, 15, kSemibold);
        d.text(name, Rect{r.x + 64, r.y + 10, nw + 4, 22}, 3, 15, kSemibold, pal.text);
        const float ww = d.R.measure(word, 0, 11, kSemibold) + 16;
        const Rect chip{r.x + 64 + nw + 10, r.y + 12, ww, 18};
        d.fill(chip, 9, alpha(hue, pal.dark ? 0.2f : 0.12f));
        d.text(word, chip, 0, 11, kSemibold, hue, DWRITE_TEXT_ALIGNMENT_CENTER);
        d.para(it.sentence, Rect{r.x + 64, r.y + 35, r.w - 76, 52}, 13, pal.text);
        d.text(widen(it.detail), Rect{r.x + 64, r.y + r.h - 22, r.w - 76, 16}, 0, 11, kNormal, pal.text3);
        if (i + 1 < l.rows.size() && !lit) d.hline(r.x + 14, r.right() - 14, r.bottom() - 1, pal.stroke2);
    }
    if (l.look_hidden > 0)
        d.text(std::format(L"Scroll down to see {} more  ↓", l.look_hidden), Rect{l.look_list.x, l.look_list.bottom() + 2, l.look_list.w, 18},
               0, 12, kNormal, pal.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
    d.icon_button(Control::Watch, l.watch_button, L'\xE768', L"Watch it run", !m.importing && !m.nodes.empty(), false);
    d.button(Control::NextStep, l.next_step, L"Next: break it  →", !m.importing && !m.toggles.empty(), true);
}

// ---- step 3: break it
void draw_changed_line(Draw& d, const Rect& box, const std::string& s, const std::string& other, bool removed, int line) {
    const Palette& pal = d.pal;
    d.fill(box, 6, d.code_fill());
    d.stroke(box, 6, pal.card_stroke);
    // what differs from the other version: the same start and end are left alone
    std::size_t p = 0;
    while (p < s.size() && p < other.size() && s[p] == other[p]) ++p;
    std::size_t q = 0;
    while (q < s.size() - p && q < other.size() - p && s[s.size() - 1 - q] == other[other.size() - 1 - q]) ++q;
    // the line without its indent, so short lines do not sit far right
    std::size_t indent = 0;
    while (indent < s.size() && indent < other.size() && s[indent] == ' ' && other[indent] == ' ') ++indent;
    const std::string shown = s.substr(indent);
    const float cw = d.char_width(14);
    const float x = box.x + 16;
    if (s.size() - q > p) {
        const float hx = x + static_cast<float>(p - indent) * cw;
        const float hw = static_cast<float>(s.size() - q - p) * cw;
        const D2D1_COLOR_F hue = removed ? pal.error : pal.ok;
        d.fill(Rect{hx - 2, box.y + 7, hw + 4, box.h - 14}, 4, alpha(hue, pal.dark ? 0.32f : 0.2f));
        if (removed) d.hline(hx, hx + hw, box.y + box.h / 2, hue);
    }
    bool in_block = false;
    d.code(shown, lex_line(shown, in_block), x, box.y, box.h, 14, cw, box.right() - 70);
    d.text(std::format(L"line {}", line), Rect{box.right() - 70, box.y, 56, box.h}, 0, 11, kNormal, pal.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void draw_output(Draw& d, const Rect& r, const wchar_t* title, const CompileResult& res, const CompileResult* compare) {
    const Palette& pal = d.pal;
    d.fill(r, 6, d.code_fill());
    d.stroke(r, 6, pal.card_stroke);
    const Rect head{r.x + 1, r.y + 1, r.w - 2, 28};
    d.fill(head, 5, alpha(pal.text, pal.dark ? 0.05f : 0.035f));
    d.hline(r.x + 1, r.right() - 1, head.bottom(), pal.card_stroke);
    d.glyph(L'\xE756', Rect{head.x + 8, head.y, 18, head.h}, 12, pal.text3);
    d.text(title, Rect{head.x + 30, head.y, head.w - 40, head.h}, 0, 12, kSemibold, pal.text2);
    const Rect inner{r.x + 12, head.bottom() + 8, r.w - 24, r.bottom() - head.bottom() - 16};
    if (!res.compiled) {
        d.text(L"(it didn't build, so nothing ran)", Rect{inner.x, inner.y, inner.w, 18}, 0, 12, kNormal, pal.text3);
        return;
    }
    const std::vector<std::string> lines = split_lines(res.output);
    const std::vector<std::string> other = compare ? split_lines(compare->output) : std::vector<std::string>();
    d.rt->PushAxisAlignedClip(rf(Rect{r.x, head.bottom(), r.w, r.bottom() - head.bottom()}), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = inner.y;
    const bool empty = res.output.empty();
    if (empty) {
        d.text(L"(printed nothing)", Rect{inner.x, y, inner.w, 18}, 0, 12, kNormal, pal.text3);
        y += 20;
    }
    for (std::size_t i = 0; !empty && i < lines.size() && y < inner.bottom(); ++i) {
        if (lines[i].empty() && i + 1 == lines.size()) break;
        const bool differs = compare && (i >= other.size() || other[i] != lines[i]);
        if (differs) {
            d.fill(Rect{r.x + 1, y - 1, r.w - 2, 20}, 0, alpha(pal.purple, pal.dark ? 0.22f : 0.14f));
            d.fill(Rect{r.x + 1, y - 1, 3, 20}, 0, pal.purple);
        }
        d.text(widen(lines[i]), Rect{inner.x, y, inner.w, 18}, 3, 12.5f, kNormal, pal.text);
        y += 20;
    }
    if (res.timed_out) d.text(L"(got stuck, stopped after 5 seconds)", Rect{inner.x, y, inner.w, 18}, 0, 12, kSemibold, pal.error);
    else if (res.crashed) d.text(L"(crashed here)", Rect{inner.x, y, inner.w, 18}, 0, 12, kSemibold, pal.error);
    d.rt->PopAxisAlignedClip();
}

void draw_result(Draw& d, const MutationToggle& t, const Challenge& ch) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    if (ch.truth < 0) {
        d.fill(l.banner, 8, pal.error_fill);
        d.circle(Rect{l.banner.x + 16, l.banner.y + 14, 36, 36}, pal.error);
        d.text(L"!", Rect{l.banner.x + 16, l.banner.y + 14, 36, 36}, 0, 18, kSemibold, pal.dark ? color(0x000000) : color(0xFFFFFF),
               DWRITE_TEXT_ALIGNMENT_CENTER);
        d.text(L"This program doesn't build", Rect{l.banner.x + 66, l.banner.y + 10, l.banner.w - 80, 24}, 1, 18, kSemibold, pal.text);
        d.text(L"Even before we break it.", Rect{l.banner.x + 66, l.banner.y + 34, l.banner.w - 80, 20}, 0, 13, kNormal, pal.text2);
        d.para("The computer said: \"" + plain_error(m.original.compile_errors) + "\"", l.result_text, 14, pal.text);
        return;
    }
    const bool right = ch.guess == ch.truth;
    const Outcome truth = static_cast<Outcome>(ch.truth);
    const D2D1_COLOR_F hue = right ? pal.ok : pal.warn;
    d.fill(l.banner, 8, alpha(hue, pal.dark ? 0.18f : 0.12f));
    d.stroke(l.banner, 8, alpha(hue, 0.35f));
    d.circle(Rect{l.banner.x + 16, l.banner.y + 14, 36, 36}, hue);
    const D2D1_COLOR_F on_hue = pal.dark ? color(0x000000) : color(0xFFFFFF);
    if (right) d.glyph(L'\xE73E', Rect{l.banner.x + 16, l.banner.y + 14, 36, 36}, 16, on_hue);
    else d.text(L"!", Rect{l.banner.x + 16, l.banner.y + 14, 36, 36}, 0, 18, kSemibold, on_hue, DWRITE_TEXT_ALIGNMENT_CENTER);
    d.text(right ? L"You got it!" : L"Not quite!", Rect{l.banner.x + 66, l.banner.y + 9, l.banner.w - 80, 26}, 1, 20, kSemibold, pal.text);
    d.text(widen(std::string("The answer: ") + outcome_title(truth) + "."), Rect{l.banner.x + 66, l.banner.y + 35, l.banner.w - 80, 20}, 0, 13,
           kNormal, pal.text2);
    d.para(explain_result(t, truth, ch.result), l.result_text, 14, pal.text);
    // why it matters: the same bug in a real game or app
    const Rect& rl = l.real_life;
    d.fill(rl, 8, alpha(pal.teal, pal.dark ? 0.12f : 0.08f));
    d.fill(Rect{rl.x, rl.y + 8, 3, rl.h - 16}, 1.5f, pal.teal);
    d.text(L"IN REAL LIFE", Rect{rl.x + 14, rl.y + 6, 120, 14}, 0, 10, kSemibold, pal.teal);
    d.para(real_life(t), Rect{rl.x + 14, rl.y + 20, rl.w - 24, rl.h - 22}, 12.5f, pal.text);
    draw_output(d, l.out_before, L"Before, it printed", m.original, nullptr);
    draw_output(d, l.out_after, L"After, it printed", ch.result, &m.original);
}

void draw_break(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    d.card(l.card_a);
    d.card(l.card_b);
    if (m.challenges.empty()) {
        d.header(l.card_a, L"Break it", L"Change one thing, then see what happens.");
        const Rect area{l.card_a.x, l.card_a.y + 80, l.card_a.w, l.card_a.h - 80};
        if (m.importing) d.empty_state(area, L'\xE895', pal.accent, L"Reading your program…", L"The challenges appear in a moment.");
        else if (m.source_path.empty()) d.empty_state(area, L'\xE8E5', pal.accent, L"Pick a program first", L"Go to step 1 and choose one of the examples.");
        else d.empty_state(area, L'\xE945', pal.orange, L"Nothing to break here", L"Try one of the example programs in step 1.");
        d.header(l.card_b, L"What really happened", L"");
        return;
    }

    const std::size_t at = static_cast<std::size_t>(std::clamp(m.current, 0, static_cast<int>(m.challenges.size()) - 1));
    const MutationToggle& t = m.toggles[at];
    const Challenge& ch = m.challenges[at];
    const Rect& a = l.card_a;

    // ---- the change
    const Quest& q = m.quest;
    std::wstring counter = std::format(L"CHALLENGE {} OF {}", at + 1, m.challenges.size());
    if (q.active) counter = L"GUESS WHAT HAPPENS";
    const float cwid = d.R.measure(counter, 0, 11, kSemibold) + 20;
    const Rect chip{a.x + 24, a.y + 22, cwid, 20};
    if (q.active && q.boss) {
        // the Bug Boss: its health, the combo, and the optional timer
        const Rect& hud = l.boss_hud;
        d.glyph(L'\xEBE8', Rect{hud.x, hud.y, 24, hud.h}, 18, pal.error);
        d.text(L"BUG BOSS", Rect{hud.x + 30, hud.y, 80, hud.h}, 1, 13, kSemibold, pal.error);
        for (int i = 0; i < q.boss_max; ++i) {
            const Rect pip{hud.x + 116 + static_cast<float>(i) * 30, hud.y + 9, 24, 14};
            d.fill(pip, 4, i < q.boss_hp ? pal.error : alpha(pal.text, 0.15f));
        }
        if (q.combo >= 2) {
            const std::wstring combo = std::format(L"COMBO x{}", q.combo);
            const float cw2 = d.R.measure(combo, 1, 12, kSemibold) + 20;
            const Rect cr{hud.x + 116 + static_cast<float>(q.boss_max) * 30 + 8, hud.y + 4, cw2, 24};
            d.fill(cr, 12, d.gold());
            d.text(combo, cr, 1, 12, kSemibold, color(0x000000), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        const Rect& tr = l.boss_timer;
        d.fill(tr, 14, q.timer ? alpha(pal.error, 0.18f) : alpha(pal.text, 0.06f));
        d.stroke(tr, 14, q.timer ? pal.error : pal.card_stroke);
        const std::wstring tl = q.timer ? std::format(L"{:.0f} s", std::ceil(std::max(0.0f, q.time_left))) : std::wstring(L"Timer off");
        d.glyph(L'\xE916', Rect{tr.x + 8, tr.y, 18, tr.h}, 12, q.timer ? pal.error : pal.text2);
        d.text(tl, Rect{tr.x + 28, tr.y, tr.w - 36, tr.h}, 0, 12, kSemibold, q.timer ? pal.error : pal.text2);
    } else {
        d.fill(chip, 10, alpha(pal.accent, pal.dark ? 0.2f : 0.12f));
        d.text(counter, chip, 0, 11, kSemibold, pal.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // one dot per challenge: done ones filled
    float dx = chip.right() + 12;
    for (std::size_t i = 0; i < m.challenges.size() && dx < l.prev_button.x - 16 && !q.active; ++i) {
        const Challenge& c = m.challenges[i];
        D2D1_COLOR_F dc = alpha(pal.text, 0.18f);
        if (c.ran && c.truth >= 0) dc = c.guess == c.truth ? pal.ok : pal.warn;
        const float size = i == at ? 10.0f : 7.0f;
        d.circle(Rect{dx, chip.y + (chip.h - size) / 2, size, size}, dc);
        if (i == at) d.stroke(Rect{dx - 2, chip.y + (chip.h - size) / 2 - 2, size + 4, size + 4}, (size + 4) / 2, pal.accent, 1.5f);
        dx += size + 8;
    }
    d.text(widen(t.label), Rect{a.x + 24, a.y + 50, a.w - 48 - (q.active ? 0.0f : 96.0f), 32}, 1, 22, kSemibold, pal.text);
    d.button(Control::PrevChallenge, l.prev_button, std::wstring(1, L'\xE76B'), m.challenges.size() > 1 && !m.running, false);
    d.button(Control::NextChallenge, l.next_button, std::wstring(1, L'\xE76C'), m.challenges.size() > 1 && !m.running, false);

    const std::string before = line_before(m.source_code, t);
    const std::string after = line_after(m.source_code, t);
    d.text(L"BEFORE", Rect{l.before_box.x, l.before_box.y - 22, l.before_box.w, 18}, 0, 11, kSemibold, pal.text3);
    draw_changed_line(d, l.before_box, before, after, true, t.line);
    d.text(L"AFTER OUR CHANGE", Rect{l.after_box.x, l.after_box.y - 22, l.after_box.w, 18}, 0, 11, kSemibold, pal.text3);
    draw_changed_line(d, l.after_box, after, before, false, t.line);
    d.glyph(L'\xE82F', Rect{l.why_text.x, l.why_text.y, 20, 22}, 14, pal.warn);
    d.para(t.description, Rect{l.why_text.x + 28, l.why_text.y + 1, l.why_text.w - 28, l.why_text.h}, 14, pal.text);

    // ---- the guess
    d.text(L"What do you think will happen?", l.question, 1, 18, kSemibold, pal.text);
    const bool can_guess = ch.guess < 0 && !m.running;
    for (int i = 0; i < 4; ++i) {
        const Rect& r = l.guesses[static_cast<std::size_t>(i)];
        const Outcome o = static_cast<Outcome>(i);
        const D2D1_COLOR_F hue = outcome_hue(pal, o);
        const bool chosen = ch.guess == i;
        const bool truth = ch.ran && ch.truth == i;
        d.tile(r, can_guess && d.hot(Control::Guess, i), can_guess && d.down(Control::Guess, i), can_guess, hue);
        float dim = 1.0f;
        if (ch.guess >= 0 && !chosen && !truth) dim = 0.4f;
        if (truth) d.stroke(r, 8, pal.ok, 2.0f);
        else if (chosen && ch.ran) d.stroke(r, 8, pal.error, 2.0f);
        else if (chosen) d.stroke(r, 8, pal.accent, 2.0f);
        const float mid = r.y + r.h / 2;
        d.badge(Rect{r.x + 16, mid - 20, 40, 40}, 20, alpha(hue, dim), outcome_mark(o), false, 19);
        d.text(widen(outcome_title(o)), Rect{r.x + 68, mid - 22, r.w - 80, 24}, 0, 15, kSemibold, alpha(pal.text, dim));
        d.text(widen(outcome_sub(o)), Rect{r.x + 68, mid + 2, r.w - 80, 20}, 0, 13, kNormal, alpha(pal.text2, dim));
        const Rect corner{r.right() - 30, r.y + 8, 22, 22};
        if (truth) {
            d.circle(corner, pal.ok);
            d.glyph(L'\xE73E', corner, 11, pal.dark ? color(0x000000) : color(0xFFFFFF));
        } else if (chosen && ch.ran) {
            d.circle(corner, pal.error);
            d.glyph(L'\xE711', corner, 10, pal.dark ? color(0x000000) : color(0xFFFFFF));
        } else if (chosen) {
            const Rect tag{r.right() - 86, r.y + 8, 78, 18};
            d.fill(tag, 9, alpha(pal.accent, pal.dark ? 0.22f : 0.13f));
            d.text(L"Your guess", tag, 0, 10, kSemibold, pal.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // ---- what really happened
    const Rect& b = l.card_b;
    d.header(b, L"What really happened", L"We build and run both versions for real.");
    const Rect body{b.x, b.y + 80, b.w, b.h - 80};
    if (ch.guess < 0) {
        d.empty_state(body, L'\xE897', pal.purple, L"Make your guess first", L"Pick one of the four answers on the left. Then we'll run the program and find out.");
        return;
    }
    if (!ch.ran) {
        d.empty_state(body, L'\xE895', pal.accent, L"Building and running…", L"This takes a few seconds.");
        return;
    }
    d.appear(m.result_anim, 12, [&] { draw_result(d, t, ch); });
    const bool last = at + 1 == m.challenges.size();
    d.icon_button(Control::WatchChanged, l.watch_changed, L'\xE768', L"Watch it run", ch.truth >= 0 && ch.result.compiled, false);
    std::wstring next = last ? L"Back to challenge 1" : L"Next challenge  →";
    if (q.active) next = q.at + 1 >= static_cast<int>(q.steps.size()) || (q.boss && q.boss_hp <= 0) ? L"Finish the quest  →" : L"Next step  →";
    d.button(Control::NextAfterResult, l.next_challenge, next, true, true);
}

// ---- the badges
void draw_badges(Draw& d) {
    const Palette& pal = d.pal;
    const Layout& l = d.l;
    const Progress& p = d.m.progress;
    d.card(l.card_a);
    d.header(l.card_a, L"Your badges", L"Earn them by guessing right. They stay, even after you close CppSparring.");
    const struct {
        std::wstring value;
        const wchar_t* label;
        wchar_t g;
        D2D1_COLOR_F hue;
    } stats[] = {{std::format(L"{} / {}", total_stars(p), examples().size() * 3), L"Stars", L'\xE735', d.gold()},
                 {std::to_wstring(p.correct), L"Right guesses", L'\xE73E', pal.ok},
                 {std::to_wstring(p.best_streak), L"Best streak", L'\xE945', pal.accent},
                 {std::format(L"{} / {}", p.badges.size(), all_badges().size()), L"Badges", L'\xE734', pal.purple}};
    for (std::size_t i = 0; i < 4; ++i) {
        const Rect& r = l.stats[i];
        d.fill(r, 8, alpha(stats[i].hue, pal.dark ? 0.12f : 0.08f));
        d.stroke(r, 8, alpha(stats[i].hue, 0.3f));
        d.badge(Rect{r.x + 16, r.y + 18, 44, 44}, 22, stats[i].hue, std::wstring(1, stats[i].g), true, 18);
        d.text(stats[i].value, Rect{r.x + 72, r.y + 14, r.w - 84, 32}, 1, 24, kSemibold, pal.text);
        d.text(stats[i].label, Rect{r.x + 72, r.y + 46, r.w - 84, 20}, 0, 13, kNormal, pal.text2);
    }
    const D2D1_COLOR_F hues[] = {pal.accent, pal.ok, pal.purple, pal.orange, pal.teal, d.gold()};
    const auto& all = all_badges();
    for (std::size_t i = 0; i < l.badge_tiles.size() && i < all.size(); ++i) {
        const Badge& b = all[i];
        const Rect& r = l.badge_tiles[i];
        const bool got = p.badges.count(b.id) > 0;
        const D2D1_COLOR_F hue = got ? hues[i % 6] : pal.text3;
        d.fill(r, 8, got ? alpha(hue, pal.dark ? 0.1f : 0.06f) : alpha(pal.text, pal.dark ? 0.03f : 0.02f));
        d.stroke(r, 8, got ? alpha(hue, 0.35f) : pal.card_stroke, 1.0f, got ? nullptr : d.R.dash.Get());
        const float mid = r.y + r.h / 2;
        const Rect medal{r.x + 14, mid - 24, 48, 48};
        if (got) {
            d.circle(medal, hue);
            const D2D1_COLOR_F on = pal.dark ? color(0x000000) : color(0xFFFFFF);
            if (b.glyph) d.glyph(b.mark[0], medal, 20, on);
            else d.text(b.mark, medal, 1, 20, kSemibold, on, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            d.circle(medal, alpha(pal.text, 0.08f));
            d.glyph(L'\xE72E', medal, 16, pal.text3);
        }
        d.text(widen(b.name), Rect{r.x + 74, mid - 22, r.w - 84, 22}, 1, 15, kSemibold, got ? pal.text : pal.text2);
        d.para(b.how, Rect{r.x + 74, mid + 1, r.w - 84, 36}, 12, got ? pal.text2 : pal.text3);
    }
}

// A badge or a new level, popping in under the title bar for a few seconds.
void draw_cheer(Draw& d) {
    const Model& m = d.m;
    if (m.cheer_t < 0 || m.cheers.empty()) return;
    const Palette& pal = d.pal;
    const Cheer& c = m.cheers.front();
    const float in = ease(m.cheer_t / 0.3f);
    const float out = m.cheer_t > 2.9f ? ease((m.cheer_t - 2.9f) / 0.35f) : 0.0f;
    const Rect& r = d.l.toast;
    d.faded(in * (1 - out), (1 - in) * -18 - out * 12, [&] {
        d.shadow(r, 12, 0.12f);
        d.fill(r, 12, pal.dark ? color(0x2F2F33) : color(0xFFFFFF));
        d.stroke(r, 12, alpha(d.gold(), 0.6f), 1.5f);
        const Rect medal{r.x + 14, r.y + 12, 44, 44};
        d.circle(medal, d.gold());
        if (c.glyph) d.glyph(c.mark.empty() ? L'\xE734' : c.mark[0], medal, 18, color(0x000000));
        else d.text(c.mark, medal, 1, 19, kSemibold, color(0x000000), DWRITE_TEXT_ALIGNMENT_CENTER);
        d.text(widen(c.title), Rect{r.x + 70, r.y + 12, r.w - 84, 24}, 1, 16, kSemibold, pal.text);
        d.text(widen(c.sub), Rect{r.x + 70, r.y + 36, r.w - 84, 20}, 0, 12, kNormal, pal.text2);
    });
}

// ---- the write-it lessons (from CodingTeacher)
const Course& course_for(int number) {
    for (const Course& c : courses())
        if (c.number == number) return c;
    return courses().front();
}

void draw_lessons(Draw& d) {
    const Palette& pal = d.pal;
    const Layout& l = d.l;
    const Progress& p = d.m.progress;
    d.card(l.card_a);
    d.header(l.card_a, L"Write-it lessons", L"Read a little, write a little C, and the real compiler checks it. From your CodingTeacher lessons.");
    const auto& all = lessons();
    if (all.empty()) {
        d.empty_state(l.lessons_area, L'\xE7BE', pal.orange, L"No lessons found", L"Put the lessons folder next to cppsparring.exe.");
        return;
    }
    int done = 0;
    for (const Lesson& ls : all) done += p.lessons_done.count(ls.id) > 0;
    d.text(std::format(L"{} of {} done", done, all.size()), Rect{l.card_a.right() - 224, l.card_a.y + 24, 200, 24}, 1, 14, kSemibold, pal.text2,
           DWRITE_TEXT_ALIGNMENT_TRAILING);
    int next = -1;
    for (std::size_t i = 0; i < all.size() && next < 0; ++i)
        if (!p.lessons_done.count(all[i].id)) next = static_cast<int>(i);

    d.rt->PushAxisAlignedClip(rf(l.lessons_area), D2D1_ANTIALIAS_MODE_ALIASED);
    for (const auto& [r, number] : l.course_heads) {
        if (r.bottom() < l.lessons_area.y || r.y > l.lessons_area.bottom()) continue;
        int n = 0, got = 0;
        for (const Lesson& ls : all)
            if (ls.course == number) {
                ++n;
                got += p.lessons_done.count(ls.id) > 0;
            }
        const Rect chip{r.x, r.y + 5, 74, 20};
        d.fill(chip, 10, alpha(pal.accent, pal.dark ? 0.2f : 0.12f));
        d.text(std::format(L"COURSE {}", number), chip, 0, 10, kSemibold, pal.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
        d.text(widen(course_for(number).name), Rect{chip.right() + 10, r.y, r.w - 200, r.h}, 1, 16, kSemibold, pal.text);
        d.text(std::format(L"{}/{}", got, n), Rect{r.right() - 80, r.y, 80, r.h}, 1, 13, kSemibold, got == n ? pal.ok : pal.text3,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    for (std::size_t t = 0; t < l.lesson_tiles.size(); ++t) {
        const Rect& r = l.lesson_tiles[t];
        if (r.bottom() < l.lessons_area.y || r.y > l.lessons_area.bottom()) continue;
        const Lesson& ls = all[static_cast<std::size_t>(l.lesson_items[t])];
        const bool is_done = p.lessons_done.count(ls.id) > 0;
        const bool up_next = l.lesson_items[t] == next;
        d.tile(r, d.hot(Control::LessonTile, static_cast<int>(t)), d.down(Control::LessonTile, static_cast<int>(t)), true, pal.accent);
        if (up_next) d.stroke(r, 8, pal.accent, 2.0f);
        const Rect num{r.x + 12, r.y + 12, 32, 32};
        if (is_done) {
            d.circle(num, pal.ok);
            d.glyph(L'\xE73E', num, 13, pal.dark ? color(0x000000) : color(0xFFFFFF));
        } else {
            d.circle(num, alpha(up_next ? pal.accent : pal.text, up_next ? 0.25f : 0.08f));
            d.text(std::to_wstring(ls.number), num, 1, 13, kSemibold, up_next ? pal.accent : pal.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        d.text(widen(ls.title), Rect{r.x + 54, r.y + 8, r.w - 64, 22}, 0, 14, kSemibold, pal.text);
        d.text(up_next ? L"Up next" : is_done ? L"Done" : L"Not started", Rect{r.x + 54, r.y + 29, r.w - 64, 18}, 0, 12, up_next ? kSemibold : kNormal,
               up_next ? pal.accent : is_done ? pal.ok : pal.text3);
    }
    d.rt->PopAxisAlignedClip();
    if (l.lessons_height > l.lessons_area.h)
        d.text(L"Scroll for more  ↓", Rect{l.lessons_area.x, l.card_a.bottom() - 18, l.lessons_area.w, 14}, 0, 11, kNormal, pal.text3,
               DWRITE_TEXT_ALIGNMENT_CENTER);
}

D2D1_COLOR_F editor_fill(const Palette& pal) { return pal.dark ? color(0x1F1F1F) : color(0xFAFAFA); }

// The compiler's messages without the file paths: "lesson.c:4:38: error: expected ';'" becomes "Line 4: expected ';'".
std::string friendly_errors(const std::string& out) {
    std::string result, line;
    auto flush = [&] {
        const std::size_t at = line.find(".c:");
        if (at != std::string::npos) {
            const int number = std::atoi(line.c_str() + at + 3);
            std::size_t msg = line.find(": ", at + 3);
            std::string text = msg == std::string::npos ? line : line.substr(msg + 2);
            for (const char* kind : {"error: ", "warning: ", "note: "})
                if (text.rfind(kind, 0) == 0) {
                    const std::string k(kind);
                    text = (k == "error: " ? "" : k.substr(0, k.size() - 2) + ": ") + text.substr(k.size());
                }
            result += "Line " + std::to_string(number) + ": " + text + "\n";
        } else if (!line.empty() && line.find("generated") == std::string::npos) {
            result += line + "\n";
        }
        line.clear();
    };
    for (char c : out) {
        if (c == '\n') flush();
        else if (c != '\r') line += c;
    }
    flush();
    return result;
}

// Text with code in it: a line that starts with four spaces is code.
float draw_mixed(Draw& d, const std::string& s, float x, float y, float w, D2D1_COLOR_F c) {
    float top = y;
    std::string line;
    auto flush = [&] {
        if (line.rfind("    ", 0) == 0) {
            d.text(widen(line.substr(4)), Rect{x + 8, y, w - 8, 19}, 3, 12.5f, kNormal, c);
            y += 20;
        } else if (!line.empty()) {
            const std::wstring wl = widen(line);
            const float h = d.R.text_height(wl, 0, 13.5f, kNormal, w);
            d.text(wl, Rect{x, y, w, h + 2}, 0, 13.5f, kNormal, c, DWRITE_TEXT_ALIGNMENT_LEADING, true, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            y += h + 2;
        } else {
            y += 6;
        }
        line.clear();
    };
    for (char ch : s) {
        if (ch == '\n') flush();
        else line += ch;
    }
    flush();
    return y - top;
}

void draw_lesson(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    const auto& all = lessons();
    d.card(l.card_a);
    d.card(l.card_b);
    if (m.lesson < 0 || m.lesson >= static_cast<int>(all.size())) return;
    const Lesson& ls = all[static_cast<std::size_t>(m.lesson)];
    const Rect& a = l.card_a;

    // ---- read
    const std::wstring chip_text = std::format(L"LESSON {}  ·  {}", ls.number, widen(course_for(ls.course).name));
    const float cw = d.R.measure(chip_text, 0, 10, kSemibold) + 20;
    d.fill(Rect{a.x + 24, a.y + 20, cw, 20}, 10, alpha(pal.accent, pal.dark ? 0.2f : 0.12f));
    d.text(chip_text, Rect{a.x + 24, a.y + 20, cw, 20}, 0, 10, kSemibold, pal.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
    d.text(widen(ls.title), Rect{a.x + 24, a.y + 44, a.w - 48, 32}, 1, 22, kSemibold, pal.text);
    const Rect& area = l.lesson_text;
    d.rt->PushAxisAlignedClip(rf(area), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = area.y;
    for (const std::string& para : ls.instructions) {
        const bool bullet = para.rfind("- ", 0) == 0;
        const std::wstring w = widen(bullet ? para.substr(2) : para);
        const float indent = bullet ? 18.0f : 0.0f;
        const float h = d.R.text_height(w, 0, 14, kNormal, area.w - indent);
        if (bullet) d.circle(Rect{area.x + 5, y + 8, 5, 5}, pal.accent);
        d.text(w, Rect{area.x + indent, y, area.w - indent, h + 2}, 0, 14, kNormal, pal.text, DWRITE_TEXT_ALIGNMENT_LEADING, true,
               DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += h + (bullet ? 4 : 10);
    }
    // the task, what it should print, and the hint
    auto box = [&](const wchar_t* label, D2D1_COLOR_F hue, float body_h, auto body) {
        const Rect r{area.x, y + 4, area.w, body_h + 38};
        d.fill(r, 8, alpha(hue, pal.dark ? 0.13f : 0.08f));
        d.fill(Rect{r.x, r.y + 8, 3, r.h - 16}, 1.5f, hue);
        d.text(label, Rect{r.x + 14, r.y + 8, r.w - 28, 16}, 0, 10, kSemibold, hue);
        body(Rect{r.x + 14, r.y + 28, r.w - 28, body_h});
        y = r.bottom() + 8;
    };
    const float tw = area.w - 28;
    box(L"YOUR TASK", pal.accent, d.R.text_height(widen(ls.task), 0, 14, kSemibold, tw), [&](const Rect& r) {
        d.text(widen(ls.task), r, 0, 14, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_LEADING, true, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    });
    if (!ls.expected.empty()) {
        const std::vector<std::string> lines = split_lines(ls.expected);
        box(L"IT SHOULD PRINT", pal.teal, static_cast<float>(lines.size()) * 19, [&](const Rect& r) {
            for (std::size_t i = 0; i < lines.size(); ++i)
                d.text(widen(lines[i]), Rect{r.x, r.y + static_cast<float>(i) * 19, r.w, 19}, 3, 12.5f, kNormal, pal.text);
        });
    }
    if (m.hint_shown && !ls.hint.empty()) {
        float hh = 0;
        {
            std::string line;
            for (char ch : ls.hint + "\n") {
                if (ch != '\n') {
                    line += ch;
                    continue;
                }
                hh += line.rfind("    ", 0) == 0 ? 20 : line.empty() ? 6 : d.R.text_height(widen(line), 0, 13.5f, kNormal, tw) + 2;
                line.clear();
            }
        }
        box(L"HINT", d.gold(), hh, [&](const Rect& r) { draw_mixed(d, ls.hint, r.x, r.y, r.w, pal.text); });
    }
    d.rt->PopAxisAlignedClip();

    // ---- write and run
    const Rect& b = l.card_b;
    d.header(b, L"Your code", L"Real C, built and run by a real compiler.");
    const Rect& ed = l.editor;
    d.fill(ed, 8, editor_fill(pal));
    d.stroke(ed, 8, d.hot(Control::Editor) ? alpha(pal.accent, 0.6f) : pal.card_stroke);
    if (!m.native_editor) {   // (a screenshot: the real edit box is not there, so draw what it holds)
        const std::vector<std::string> lines = split_lines(m.lesson_code);
        const float cwid = d.char_width(13);
        bool in_block = false;
        d.rt->PushAxisAlignedClip(rf(ed), D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t i = 0; i < lines.size() && ed.y + 10 + static_cast<float>(i) * 19 < ed.bottom() - 8; ++i)
            d.code(lines[i], lex_line(lines[i], in_block), ed.x + 12, ed.y + 10 + static_cast<float>(i) * 19, 19, 13, cwid, ed.right() - 8);
        d.rt->PopAxisAlignedClip();
    }
    d.icon_button(Control::LessonRun, l.run_button, L'\xE768', m.lesson_running ? L"Running…" : L"Run", !m.lesson_running, true);
    d.icon_button(Control::LessonHint, l.hint_button, L'\xE82F', m.hint_shown ? L"Hide hint" : L"Show a hint", !ls.hint.empty(), false);
    d.icon_button(Control::LessonReset, l.reset_button, L'\xE72C', L"Reset", true, false);

    // ---- what happened
    const Rect& bn = l.lesson_banner;
    const LessonCheck& chk = m.lesson_check;
    D2D1_COLOR_F hue = pal.accent;
    std::wstring title = L"Press Run when you're ready", sub = L"Nothing has run yet. Mistakes are fine. That's how everyone learns.";
    wchar_t mark = L'\xE768';
    if (m.lesson_running) {
        title = L"Building and running…";
        sub = L"The compiler is reading your code.";
        mark = L'\xE895';
    } else if (m.lesson_ran && chk.passed) {
        hue = pal.ok;
        title = L"You did it!";
        sub = ls.expected.empty() ? L"It builds and runs. Nice work." : L"It prints exactly what it should.";
        mark = L'\xE73E';
    } else if (m.lesson_ran && !chk.run.compiled) {
        hue = pal.orange;
        title = L"Not yet: the computer couldn't build it";
        sub = widen(plain_error(chk.run.compile_errors));
        mark = L'\xE7BA';
    } else if (m.lesson_ran && (chk.run.crashed || chk.run.timed_out)) {
        hue = pal.error;
        title = chk.run.timed_out ? L"Not yet: it got stuck" : L"Not yet: it crashed";
        sub = chk.run.timed_out ? L"Is there a loop that never ends?" : L"Something went badly wrong while it ran.";
        mark = L'\xE7BA';
    } else if (m.lesson_ran && ls.expected.empty()) {
        hue = pal.warn;
        title = L"It runs! Now do the task";
        sub = L"The code is still the same as when you started. Follow YOUR TASK on the left.";
        mark = L'\xE7BA';
    } else if (m.lesson_ran) {
        hue = pal.warn;
        title = L"Close! It runs, but prints something else";
        sub = L"Compare what it printed with what it should print. The different lines are lit.";
        mark = L'\xE7BA';
    }
    d.fill(bn, 8, alpha(hue, pal.dark ? 0.16f : 0.1f));
    d.stroke(bn, 8, alpha(hue, 0.35f));
    d.circle(Rect{bn.x + 12, bn.y + 9, 32, 32}, hue);
    d.glyph(mark, Rect{bn.x + 12, bn.y + 9, 32, 32}, 14, pal.dark ? color(0x000000) : color(0xFFFFFF));
    d.text(title, Rect{bn.x + 56, bn.y + 6, bn.w - 68, 22}, 1, 15, kSemibold, pal.text);
    d.text(sub, Rect{bn.x + 56, bn.y + 27, bn.w - 68, 18}, 0, 12, kNormal, pal.text2);
    if (m.lesson_ran && !m.lesson_running) {
        if (chk.run.compiled) {
            CompileResult want;
            want.compiled = true;
            want.output = ls.expected;
            draw_output(d, l.lesson_out, L"It printed", chk.run, ls.expected.empty() ? nullptr : &want);
        } else {
            const std::string said = friendly_errors(chk.run.compile_errors);
            draw_console(d, l.lesson_out, said, said.size(), L"", L"What the computer said");
        }
    }
    const Quest& q = m.quest;
    std::wstring next = L"Next lesson  →";
    if (q.active) next = q.at + 1 >= static_cast<int>(q.steps.size()) ? L"Finish the quest  →" : L"Next step  →";
    d.button(Control::LessonNext, l.lesson_next, m.lesson_ran && chk.passed ? next : std::wstring(L"Pass the task to go on"), m.lesson_ran && chk.passed, true);
}

// ---- a quick quiz card
void draw_quiz(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    const Rect& c = l.quiz_card;
    d.card(c);
    if (m.quiz < 0 || m.quiz >= static_cast<int>(quiz_cards().size())) return;
    const QuizCard& card = quiz_cards()[static_cast<std::size_t>(m.quiz)];
    const std::array<int, 4> order = answer_order(static_cast<std::size_t>(m.quiz));
    const bool answered = m.quiz_choice >= 0;
    d.fill(Rect{c.x + 32, c.y + 24, 96, 20}, 10, alpha(pal.purple, pal.dark ? 0.22f : 0.13f));
    d.text(L"QUICK QUIZ", Rect{c.x + 32, c.y + 24, 96, 20}, 0, 10, kSemibold, pal.purple, DWRITE_TEXT_ALIGNMENT_CENTER);
    d.text(widen(card.question), Rect{c.x + 32, c.y + 52, c.w - 64, 64}, 1, 22, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_LEADING, true,
           DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (card.code) {
        const Rect& cr = l.quiz_code;
        d.fill(cr, 8, d.code_fill());
        d.stroke(cr, 8, pal.card_stroke);
        const std::vector<std::string> lines = split_lines(card.code);
        const float cw = d.char_width(14);
        bool in_block = false;
        for (std::size_t i = 0; i < lines.size(); ++i)
            d.code(lines[i], lex_line(lines[i], in_block), cr.x + 16, cr.y + 10 + static_cast<float>(i) * 22, 22, 14, cw, cr.right() - 8);
    }
    const wchar_t* letters[] = {L"A", L"B", L"C", L"D"};
    for (int i = 0; i < 4; ++i) {
        const Rect& r = l.quiz_answers[static_cast<std::size_t>(i)];
        const int which = order[static_cast<std::size_t>(i)];
        const bool right = which == 0;
        const bool chosen = m.quiz_choice == i;
        d.tile(r, !answered && d.hot(Control::QuizAnswer, i), !answered && d.down(Control::QuizAnswer, i), !answered, pal.purple);
        float dim = 1.0f;
        D2D1_COLOR_F hue = pal.purple;
        if (answered) {
            if (right) {
                d.stroke(r, 8, pal.ok, 2.0f);
                hue = pal.ok;
            } else if (chosen) {
                d.stroke(r, 8, pal.error, 2.0f);
                hue = pal.error;
            } else {
                dim = 0.4f;
            }
        }
        d.badge(Rect{r.x + 14, r.y + (r.h - 36) / 2, 36, 36}, 18, alpha(hue, dim), letters[i], false, 15);
        d.text(widen(card.answers[static_cast<std::size_t>(which)]), Rect{r.x + 62, r.y, r.w - 96, r.h}, 0, 15, kSemibold, alpha(pal.text, dim),
               DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (answered && (right || chosen)) {
            const Rect corner{r.right() - 32, r.y + (r.h - 22) / 2, 22, 22};
            d.circle(corner, right ? pal.ok : pal.error);
            d.glyph(right ? L'\xE73E' : L'\xE711', corner, 10, pal.dark ? color(0x000000) : color(0xFFFFFF));
        }
    }
    if (answered) {
        const bool got = order[static_cast<std::size_t>(m.quiz_choice)] == 0;
        const Rect& w = l.quiz_why;
        d.fill(w, 8, alpha(got ? pal.ok : pal.warn, pal.dark ? 0.14f : 0.09f));
        d.text(got ? L"Yes!" : L"Not quite. Here's the trick:", Rect{w.x + 14, w.y + 8, w.w - 28, 20}, 1, 14, kSemibold, pal.text);
        d.para(card.why, Rect{w.x + 14, w.y + 30, w.w - 28, w.h - 32}, 13, pal.text2);
        const Quest& q = m.quest;
        d.button(Control::QuizNext, l.quiz_next,
                 q.active && q.at + 1 >= static_cast<int>(q.steps.size()) ? L"Finish the quest  →" : L"Next step  →", true, true);
    }
}

// ---- the end of a quest: a celebration, the treasure chest, and a choice to go again or rest
void draw_quest_done(Draw& d) {
    const Palette& pal = d.pal;
    const Model& m = d.m;
    const Layout& l = d.l;
    const Quest& q = m.quest;
    const Rect& c = l.done_card;
    d.card(c);
    int right = 0, total = 0;
    for (const QuestStep& s : q.steps) {
        total += !s.skipped;
        right += s.right;
    }
    const bool beaten = q.boss && q.boss_hp <= 0;
    const Rect medal{c.x + (c.w - 88) / 2, c.y + 28, 88, 88};
    d.shadow(medal, 44, 0.08f);
    d.circle(medal, q.boss ? (beaten ? d.gold() : pal.orange) : d.gold());
    d.glyph(q.boss ? L'\xEBE8' : L'\xE7C1', medal, 36, color(0x000000));
    const std::wstring title = q.boss ? (beaten ? L"Bug Boss beaten!" : L"The bug got away... this time") : L"Quest complete!";
    d.text(title, Rect{c.x, c.y + 128, c.w, 36}, 1, 28, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    std::wstring sub = std::format(L"{} right out of {}   ·   +{} XP", right, total, q.xp);
    if (q.boss && q.best_combo >= 2) sub += std::format(L"   ·   best combo x{}", q.best_combo);
    d.text(sub, Rect{c.x, c.y + 168, c.w, 22}, 0, 15, kNormal, pal.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (q.boss && !beaten)
        d.text(L"No worries. It'll be back, and you'll be ready.", Rect{c.x, c.y + 192, c.w, 20}, 0, 13, kNormal, pal.text3, DWRITE_TEXT_ALIGNMENT_CENTER);

    // the chest: a surprise amount of bonus XP
    const Rect& ch = l.chest;
    if (!q.chest_open) {
        d.tile(ch, d.hot(Control::ChestOpen), d.down(Control::ChestOpen), true, d.gold());
        d.stroke(ch, 8, alpha(d.gold(), 0.7f), 2.0f);
        d.glyph(L'\xE7B8', Rect{ch.x, ch.y + 18, ch.w, 60}, 44, d.gold());
        d.text(L"Open the treasure chest", Rect{ch.x, ch.y + 92, ch.w, 24}, 1, 15, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        d.fill(ch, 8, alpha(d.gold(), pal.dark ? 0.25f : 0.18f));
        d.stroke(ch, 8, d.gold(), 2.0f);
        d.text(std::format(L"+{} XP", q.chest_xp), Rect{ch.x, ch.y + 26, ch.w, 52}, 1, 40, kSemibold, pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        d.text(L"Bonus from the chest!", Rect{ch.x, ch.y + 86, ch.w, 22}, 0, 14, kNormal, pal.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const int days = days_this_week(m.progress);
    d.text(days >= kWeekGoal ? std::wstring(L"This week's goal is done. Great job showing up!")
                             : std::format(L"This week: {} of {} days. Every day you come back counts.", days, kWeekGoal),
           Rect{c.x, ch.bottom() + 20, c.w, 22}, 0, 14, kSemibold, pal.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
    d.button(Control::QuestHome, l.done_home, L"Take a break", true, false);
    d.button(Control::QuestAgain, l.done_again, L"Another quest  →", true, true);
}

// Paper bits flying out of the banner when a guess is right.
void draw_confetti(Draw& d, float x0, float y0, float t) {
    if (t < 0) return;
    const Palette& pal = d.pal;
    const D2D1_COLOR_F colors[] = {pal.accent, pal.ok, pal.purple, pal.orange, pal.teal, pal.warn};
    std::uint32_t seed = 0x2545F491u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    const float fade = t > 1.1f ? std::max(0.0f, 1.0f - (t - 1.1f) / 0.5f) : 1.0f;
    D2D1_MATRIX_3X2_F before;
    d.rt->GetTransform(&before);
    for (int i = 0; i < 80; ++i) {
        const float ang = -1.5708f + (rnd() - 0.5f) * 2.4f;
        const float speed = 240.0f + rnd() * 340.0f;
        const float spin = (rnd() - 0.5f) * 900.0f;
        const float start = rnd() * 360.0f;
        const float w = 5.0f + rnd() * 5.0f;
        const float x = x0 + std::cos(ang) * speed * t;
        const float y = y0 + std::sin(ang) * speed * t + 0.5f * 950.0f * t * t;
        d.rt->SetTransform(D2D1::Matrix3x2F::Rotation(start + spin * t) * D2D1::Matrix3x2F::Translation(x, y) * before);
        d.fill(Rect{-w / 2, -2, w, 4}, 1, alpha(colors[i % 6], fade));
    }
    d.rt->SetTransform(before);
}

}  // namespace

HRESULT Renderer::paint(ID2D1RenderTarget* rt, const Model& model, const Layout& layout, const Palette& pal, bool backdrop) {
    if (!rt) return E_POINTER;
    ComPtr<ID2D1SolidColorBrush> b;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b))) return E_FAIL;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    Draw d{*this, rt, b.Get(), pal, model, layout};
    if (!backdrop) d.fill(Rect{0, 0, layout.width, layout.height}, 0, pal.mica);
    draw_title_bar(d);
    draw_rail(d);
    d.appear(model.page_anim, 10, [&] {
        switch (model.page) {
        case Page::Home: draw_home(d); break;
        case Page::Practice: draw_practice(d); break;
        case Page::Look: draw_look(d); break;
        case Page::Break: draw_break(d); break;
        case Page::Badges: draw_badges(d); break;
        case Page::Lessons: draw_lessons(d); break;
        case Page::Lesson: draw_lesson(d); break;
        case Page::Quiz: draw_quiz(d); break;
        case Page::QuestDone: draw_quest_done(d); break;
        }
    });
    draw_quest_bar(d);
    draw_session_bar(d);
    draw_status(d);
    if (model.page == Page::Break) draw_confetti(d, layout.banner.x + 34, layout.banner.y + 32, model.confetti);
    else if (model.confetti >= 0) draw_confetti(d, layout.toast.x + 36, layout.toast.y + 34, model.confetti);
    draw_cheer(d);
    return rt->EndDraw();
}

}  // namespace sparring
