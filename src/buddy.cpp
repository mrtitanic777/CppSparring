#include "buddy.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "layout.hpp"
#include "palette.hpp"
#include "render.hpp"

namespace sparring {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHopSeconds = 0.42f;
constexpr float kEnterSeconds = 0.55f;
constexpr float kLeaveSeconds = 0.5f;
constexpr float kWaveSeconds = 1.6f;
constexpr float kBlinkSeconds = 0.15f;

const wchar_t* const kChipText[kBuddyChips] = {L"Explain this line", L"Give me a hint", L"What went wrong?", L"Read it to me"};

float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
float ease_in_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }

unsigned next_random(unsigned& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}
float random01(unsigned& s) { return static_cast<float>(next_random(s) % 10000) / 10000.0f; }

D2D1_RECT_F rf(const BuddyRect& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

void start_hop(Buddy& b, float x, float y, float height) {
    b.from_x = b.x;
    b.from_y = b.y;
    b.to_x = x;
    b.to_y = y;
    b.hop = 0;
    b.hop_height = height;
}

// Where Bit stands to point at the lines: on the edge of the reading card, level with them.
void perch(const Buddy& b, const Layout& l, float& x, float& y) {
    x = l.card_a.right() - 4;
    y = std::clamp(b.target.y + b.target.h / 2 + 44, l.card_a.y + 120, l.card_a.bottom() - 12);
}

// ---- drawing helpers
struct Pen {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br;
    Renderer& r;
    void fill_ellipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), br);
    }
    void fill_round(float x0, float y0, float x1, float y1, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), rad, rad), br);
    }
    void stroke_round(float x0, float y0, float x1, float y1, float rad, D2D1_COLOR_F c, float w) {
        br->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), rad, rad), br, w);
    }
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w, ID2D1StrokeStyle* s = nullptr) {
        br->SetColor(c);
        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), br, w, s);
    }
    void text(const std::wstring& s, const BuddyRect& box, float size, int weight, D2D1_COLOR_F c, DWRITE_TEXT_ALIGNMENT align, bool wrap,
              int kind = 0, DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (s.empty() || box.w <= 0 || box.h <= 0) return;
        IDWriteTextFormat* f = r.format(kind, size, weight, align, v, wrap);
        if (!f) return;
        br->SetColor(c);
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, rf(box), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
};

ID2D1Factory* factory_of(ID2D1RenderTarget* rt) {
    ID2D1Factory* f = nullptr;
    rt->GetFactory(&f);
    return f;   // (a reference is held; released by the caller)
}

// A line drawn only as far as `t` (0..1) along it, with round ends.
void draw_partial(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, ID2D1Geometry* g, float t, float width, D2D1_COLOR_F c, float dash_offset = -1) {
    ID2D1Factory* f = factory_of(rt);
    if (!f) return;
    float len = 0;
    g->ComputeLength(nullptr, &len);
    ID2D1StrokeStyle* style = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
    float dashes[2];
    UINT count = 2;
    if (dash_offset >= 0) {   // marching dashes
        dashes[0] = 2.2f;
        dashes[1] = 1.8f;
        props.dashOffset = dash_offset;
    } else {
        dashes[0] = std::max(0.001f, len * std::clamp(t, 0.0f, 1.0f) / width);
        dashes[1] = len / width + 10;
    }
    if (dash_offset < 0 && t >= 1) {
        props.dashStyle = D2D1_DASH_STYLE_SOLID;
        count = 0;
    }
    f->CreateStrokeStyle(props, count ? dashes : nullptr, count, &style);
    br->SetColor(c);
    rt->DrawGeometry(g, br, width, style);
    if (style) style->Release();
    f->Release();
}

// A twinkling four-pointed star.
void sparkle(Pen& p, float cx, float cy, float size, D2D1_COLOR_F c) {
    if (size <= 0.3f) return;
    p.line(cx - size, cy, cx + size, cy, c, 1.6f);
    p.line(cx, cy - size, cx, cy + size, c, 1.6f);
    p.fill_ellipse(cx, cy, size * 0.35f, size * 0.35f, c);
}

struct Pose {
    float x = 0, y = 0;     // feet, in the window
    float sx = 1, sy = 1;   // stretch
    float rot = 0;          // degrees
    float fade = 1;         // 0..1
    float lift = 0;         // 0..1, how high in a hop
    D2D1::Matrix3x2F m;
};

Pose pose_of(const Buddy& b) {
    Pose p;
    p.x = b.x;
    p.y = b.y;
    float s = 1;
    if (b.mode == BuddyMode::Entering) s = lerp(0.45f, 1.0f, ease_out(b.t / 0.18f));
    if (b.hop < 1) {
        const float arc = std::sin(kPi * b.hop);
        p.lift = arc;
        p.sy *= 1 + 0.14f * arc;
        p.sx *= 1 - 0.09f * arc;
    } else {
        // idle: breathe, sway, and drift in a little figure eight
        const float breath = std::sin(b.clock * 2 * kPi * 0.55f) * 0.03f;
        p.sy *= 1 + breath;
        p.sx *= 1 - breath * 0.7f;
        p.rot = std::sin(b.clock * 1.3f) * 3.5f;
        p.x += std::sin(b.clock * 0.9f) * 1.8f;
        p.y += std::sin(b.clock * 1.8f) * 1.1f;
    }
    p.sy *= 1 - b.squash;
    p.sx *= 1 + b.squash * 0.7f;
    if (b.mode == BuddyMode::Leaving) {
        const float t = b.t;
        if (t < 0.15f) {
            const float k = ease_out(t / 0.15f);
            p.sy *= lerp(1, 0.55f, k);
            p.sx *= lerp(1, 1.35f, k);
        } else {
            const float k = ease_in_out((t - 0.15f) / 0.3f);
            s = lerp(1, 0.0f, k);
            p.sy *= 0.55f;
            p.sx *= 1.35f;
            p.fade = 1 - k;
        }
    }
    p.sx *= s;
    p.sy *= s;
    p.m = D2D1::Matrix3x2F::Scale(p.sx, p.sy) * D2D1::Matrix3x2F::Rotation(p.rot) * D2D1::Matrix3x2F::Translation(p.x, p.y);
    return p;
}

D2D1_POINT_2F shoulder(bool right) { return D2D1::Point2F(right ? 29.0f : -29.0f, -34.0f); }

// The right arm's angle (degrees, 0 = pointing right, 90 = down), in Bit's own frame.
float right_arm_angle(const Buddy& b, const Pose& p) {
    float a = 62 + std::sin(b.clock * 1.7f) * 6;
    if (b.point_t >= 0 && b.target_shown) {
        const D2D1_POINT_2F s = p.m.TransformPoint(shoulder(true));
        const float tx = b.target.x - 4, ty = b.target.y + b.target.h / 2;
        float want = std::atan2(ty - s.y, tx - s.x) * 180 / kPi - p.rot;
        const float k = ease_out(b.point_t / 0.3f);
        a = lerp(a, want, k);
    }
    if (b.wave >= 0) {
        const float env = std::sin(kPi * std::clamp(b.wave / kWaveSeconds, 0.0f, 1.0f));
        const float waving = -70 + std::sin(b.wave * 2 * kPi * 2.6f) * 28;
        a = lerp(a, waving, std::min(1.0f, env * 1.6f));
    }
    return a;
}

void draw_arm(Pen& p, bool right, float angle_deg, D2D1_COLOR_F arm, D2D1_COLOR_F hand) {
    const D2D1_POINT_2F s = shoulder(right);
    const float a = angle_deg * kPi / 180;
    const float ex = s.x + std::cos(a) * 15, ey = s.y + std::sin(a) * 15;
    p.line(s.x, s.y, ex, ey, arm, 8);
    p.fill_ellipse(ex, ey, 5.2f, 5.2f, hand);
}

void draw_bit(Pen& p, const Palette& pal, const Buddy& b, const Pose& pose) {
    ID2D1RenderTarget* rt = p.rt;
    const float f = pose.fade;
    const D2D1_COLOR_F body_top = color(pal.dark ? 0xB2A7FF : 0xA89CFF, f), body_bottom = color(pal.dark ? 0x7F70F2 : 0x7565EC, f);
    const D2D1_COLOR_F limb = color(pal.dark ? 0x6A5AE0 : 0x5F4FD6, f);
    const D2D1_COLOR_F screen = color(0x1C2140, f), glow = color(0x7CF5D8, f), cheek = color(0xFF8FB1, 0.55f * f);

    // the shadow stays on the ground
    D2D1_MATRIX_3X2_F saved;
    rt->GetTransform(&saved);
    const float shrink = 1 - pose.lift * 0.45f;
    p.fill_ellipse(b.from_x + (b.to_x - b.from_x) * std::clamp(b.hop, 0.0f, 1.0f), (b.hop < 1 ? lerp(b.from_y, b.to_y, b.hop) : b.y) + 1, 26 * shrink * pose.sx,
                   4.5f * shrink, color(0x000000, (pal.dark ? 0.35f : 0.16f) * f * (1 - pose.lift * 0.5f)));
    rt->SetTransform(pose.m * saved);

    // feet and the left arm (behind the body)
    p.fill_ellipse(-13, -3, 9, 5, limb);
    p.fill_ellipse(13, -3, 9, 5, limb);
    draw_arm(p, false, 118 + std::sin(b.clock * 1.7f + 1) * 6, limb, body_bottom);

    // the antenna wobbles a little behind the sway
    const float wob = std::sin(b.clock * 3.1f) * 5 - pose.rot * 0.9f;
    const float tipx = wob, tipy = -82;
    {
        ID2D1Factory* fac = factory_of(rt);
        ID2D1PathGeometry* g = nullptr;
        if (fac && SUCCEEDED(fac->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr;
            if (SUCCEEDED(g->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(0, -62), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(wob * 0.2f, -74), D2D1::Point2F(tipx, tipy)));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->Close();
                s->Release();
            }
            p.br->SetColor(limb);
            rt->DrawGeometry(g, p.br, 2.6f);
            g->Release();
        }
        if (fac) fac->Release();
    }
    if (b.listening) {
        const float pulse = 0.5f + 0.5f * std::sin(b.clock * 7);
        p.fill_ellipse(tipx, tipy, 9 + pulse * 5, 9 + pulse * 5, color(0xFF5C8A, 0.25f * f));
    }
    p.fill_ellipse(tipx, tipy, 5, 5, b.listening ? color(0xFF5C8A, f) : color(0xFFD166, f));
    p.fill_ellipse(tipx - 1.5f, tipy - 1.5f, 1.6f, 1.6f, color(0xFFFFFF, 0.8f * f));

    // the body: a soft rounded block, lighter on top
    {
        ID2D1GradientStopCollection* stops = nullptr;
        const D2D1_GRADIENT_STOP gs[2] = {{0.0f, body_top}, {1.0f, body_bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ID2D1LinearGradientBrush* grad = nullptr;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, -64), D2D1::Point2F(0, -6)), stops, &grad);
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(D2D1::RectF(-30, -64, 30, -6), 24, 24);
        if (grad) rt->FillRoundedRectangle(shape, grad);
        else p.fill_round(-30, -64, 30, -6, 24, body_bottom);
        p.br->SetColor(color(0x3B2DAA, 0.35f * f));
        rt->DrawRoundedRectangle(shape, p.br, 1.4f);
        if (grad) grad->Release();
        if (stops) stops->Release();
    }
    p.fill_ellipse(-16, -55, 7, 3.5f, color(0xFFFFFF, 0.3f * f));   // a shine

    // the face: a little screen with glowing eyes
    p.fill_round(-21, -54, 21, -23, 10, screen);
    p.stroke_round(-21, -54, 21, -23, 10, color(0x7CF5D8, 0.25f * f), 1.2f);
    float lx = 0, ly = 0;
    if (b.thinking) {
        lx = -2.5f + std::sin(b.clock * 4) * 2.5f;
        ly = -2;
    } else {
        D2D1::Matrix3x2F inv = pose.m;
        if (inv.Invert()) {
            const D2D1_POINT_2F local = inv.TransformPoint(D2D1::Point2F(b.look_x, b.look_y));
            const float dx = local.x, dy = local.y + 40;
            const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
            lx = dx / len * std::min(3.2f, len / 40);
            ly = dy / len * std::min(2.2f, len / 40);
        }
    }
    float open = 1;
    if (b.blink >= 0) open = 1 - std::sin(kPi * std::clamp(b.blink / kBlinkSeconds, 0.0f, 1.0f)) * 0.9f;
    const bool happy = b.wave >= 0;
    for (float ex : {-9.0f, 9.0f}) {
        const float cx = ex + lx, cy = -38.5f + ly;
        if (happy) {   // ^ ^
            p.line(cx - 4, cy + 1.5f, cx, cy - 3, glow, 2.4f);
            p.line(cx, cy - 3, cx + 4, cy + 1.5f, glow, 2.4f);
        } else {
            const BuddyMood face = buddy_shown_mood(b);
            const float h = 5.3f * open * (face == BuddyMood::Surprised ? 1.15f : face == BuddyMood::Worried ? 0.9f : 1.0f);
            p.fill_round(cx - 3.6f, cy - h, cx + 3.6f, cy + h, std::min(3.6f, h), glow);
            if (open > 0.6f) p.fill_ellipse(cx + 1.2f, cy - 2.4f, 1.1f, 1.1f, color(0xFFFFFF, 0.85f * f));
        }
    }
    // the mouth follows the voice; a small smile when quiet
    if (b.mouth > 0.06f) {
        p.fill_ellipse(lx * 0.5f, -28.8f, 3.6f + b.mouth * 1.2f, 1.2f + b.mouth * 3.4f, glow);
    } else {
        p.line(-3.5f + lx * 0.5f, -30, -1.2f + lx * 0.5f, -28.4f, glow, 1.8f);
        p.line(-1.2f + lx * 0.5f, -28.4f, 1.2f + lx * 0.5f, -28.4f, glow, 1.8f);
        p.line(1.2f + lx * 0.5f, -28.4f, 3.5f + lx * 0.5f, -30, glow, 1.8f);
    }
    // the eyebrows: they show the mood, bob while it talks, and flick up now and then
    {
        const BuddyMood mood = buddy_shown_mood(b);
        const float arch = mood == BuddyMood::Happy ? 1.6f : mood == BuddyMood::Surprised ? 1.3f : 0.5f;
        float extra = -b.mouth * 1.4f;
        if (b.brow_flick >= 0) extra -= 2.6f * std::sin(kPi * std::clamp(b.brow_flick / 0.35f, 0.0f, 1.0f));
        for (int i = 0; i < 2; ++i) {
            const float cx = (i == 0 ? -9.0f : 9.0f) + lx * 0.6f, cy = -47.4f + b.brow_lift[i] + extra + ly * 0.3f;
            const float a = b.brow_tilt[i] * kPi / 180, half = 4.7f;
            const float dx = std::cos(a) * half, dy = std::sin(a) * half;
            const float mx = cx + std::sin(a) * arch, my = cy - std::cos(a) * arch;   // the middle, bowed up
            p.line(cx - dx, cy - dy, mx, my, glow, 2.6f);
            p.line(mx, my, cx + dx, cy + dy, glow, 2.6f);
            for (const auto& [px, py] : {std::pair{cx - dx, cy - dy}, std::pair{mx, my}, std::pair{cx + dx, cy + dy}}) p.fill_ellipse(px, py, 1.3f, 1.3f, glow);
        }
    }
    p.fill_ellipse(-25, -21, 4.5f, 2.8f, cheek);
    p.fill_ellipse(25, -21, 4.5f, 2.8f, cheek);

    // the right arm (in front): it hangs, waves, or points
    draw_arm(p, true, right_arm_angle(b, pose), limb, body_bottom);
    rt->SetTransform(saved);

    // a poof when it plops out
    if (b.mode == BuddyMode::Leaving && b.t > 0.12f) {
        const float k = std::clamp((b.t - 0.12f) / 0.38f, 0.0f, 1.0f);
        for (int i = 0; i < 7; ++i) {
            const float a = static_cast<float>(i) / 7 * 2 * kPi + 0.4f;
            const float d = 10 + 34 * ease_out(k);
            p.fill_ellipse(b.x + std::cos(a) * d, b.y - 32 + std::sin(a) * d * 0.8f, 7 * (1 - k) + 1, 7 * (1 - k) + 1, color(pal.dark ? 0xD9D2FF : 0xB9AEFF, 0.8f * (1 - k)));
        }
    }
}

// The arrow from Bit's hand to the lines, and the glowing box around them.
void draw_pointing(Pen& p, const Palette& pal, const Buddy& b, const Pose& pose) {
    if (b.point_t < 0 || !b.target_shown || b.target.w <= 0) return;
    ID2D1RenderTarget* rt = p.rt;
    ID2D1Factory* fac = factory_of(rt);
    if (!fac) return;
    const D2D1_COLOR_F ink = color(pal.dark ? 0xFFD166 : 0xF29E00);
    const D2D1_COLOR_F box = color(pal.dark ? 0x7CF5D8 : 0x0FA88A);

    // the box: drawn around the lines like a pen going round, then its dashes march
    const BuddyRect t = b.target;
    const float settle = b.hop < 1 ? 0.0f : 1.0f;
    const float k_box = std::clamp((b.point_t - 0.2f) / 0.5f, 0.0f, 1.0f) * settle;
    if (k_box > 0) {
        const float pulse = 0.5f + 0.5f * std::sin(b.clock * 4);
        p.fill_round(t.x - 6, t.y - 3, t.x + t.w + 6, t.y + t.h + 3, 6, alpha(box, (0.07f + 0.06f * pulse) * k_box));
        ID2D1RoundedRectangleGeometry* g = nullptr;
        fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(t.x - 6, t.y - 3, t.x + t.w + 6, t.y + t.h + 3), 6, 6), &g);
        if (g) {
            if (k_box < 1) draw_partial(rt, p.br, g, ease_in_out(k_box), 2.2f, box);
            else draw_partial(rt, p.br, g, 1, 2.2f, box, b.clock * 3.0f);
            g->Release();
        }
        // sparkles twinkle at two corners
        sparkle(p, t.x + t.w + 6, t.y - 3, 4.5f * k_box * (0.6f + 0.4f * std::sin(b.clock * 6)), ink);
        sparkle(p, t.x - 6, t.y + t.h + 3, 3.5f * k_box * (0.6f + 0.4f * std::sin(b.clock * 6 + 2)), ink);
    }

    // the arrow: a curve from the hand that draws itself, then a head that pops on
    const float k_arrow = std::clamp((b.point_t - 0.1f) / 0.45f, 0.0f, 1.0f) * settle;
    if (k_arrow > 0) {
        const float a = right_arm_angle(b, pose) * kPi / 180;
        const D2D1_POINT_2F s = shoulder(true);
        const D2D1_POINT_2F hand = pose.m.TransformPoint(D2D1::Point2F(s.x + std::cos(a) * 21, s.y + std::sin(a) * 21));
        const D2D1_POINT_2F end = D2D1::Point2F(t.x - 10, t.y + t.h / 2);
        const float dx = end.x - hand.x;
        const D2D1_POINT_2F c1 = D2D1::Point2F(hand.x + dx * 0.35f, hand.y - 34);
        const D2D1_POINT_2F c2 = D2D1::Point2F(end.x - dx * 0.35f - 8, end.y - 26);
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(fac->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(g->Open(&sink))) {
                sink->BeginFigure(hand, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                sink->Release();
            }
            draw_partial(rt, p.br, g, ease_out(k_arrow), 3, ink);
            g->Release();
        }
        if (k_arrow >= 1) {
            const float pop = std::min(1.0f, (b.point_t - 0.55f) / 0.15f);
            const float bounce = 1 + 0.25f * std::sin(std::clamp(pop, 0.0f, 1.0f) * kPi);
            const float ang = std::atan2(end.y - c2.y, end.x - c2.x);
            const float len = 9 * bounce * std::max(0.0f, pop);
            for (float side : {-0.6f, 0.6f})
                p.line(end.x, end.y, end.x - std::cos(ang + side) * len, end.y - std::sin(ang + side) * len, ink, 3);
        }
    }
    fac->Release();
}

void draw_bubble(Pen& p, const Palette& pal, const Buddy& b, const BuddyLayout& bl, int hot_chip, bool hot_mic, bool hot_close) {
    if (b.said.empty() || bl.bubble.w <= 0) return;
    ID2D1RenderTarget* rt = p.rt;
    float fade = 1;
    if (b.mode == BuddyMode::Entering) fade = std::clamp((b.t - 0.35f) / 0.2f, 0.0f, 1.0f);
    if (b.mode == BuddyMode::Leaving) fade = std::clamp(1 - b.t / 0.15f, 0.0f, 1.0f);
    if (fade <= 0) return;
    const float pop = ease_out(std::min(1.0f, b.said_t / 0.18f));
    const BuddyRect& r = bl.bubble;
    D2D1_MATRIX_3X2_F saved;
    rt->GetTransform(&saved);
    const float s = 0.94f + 0.06f * pop;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(r.x, r.y + r.h)) * saved);

    const D2D1_COLOR_F paper = pal.dark ? color(0x34343C, fade) : color(0xFFFFFF, fade);
    const D2D1_COLOR_F edge = color(pal.dark ? 0x9D8FFF : 0x7565EC, 0.55f * fade);
    // a soft shadow, the bubble, and its tail toward Bit's head
    for (int i = 3; i >= 1; --i) {
        const float k = static_cast<float>(i);
        p.fill_round(r.x - k * 0.5f, r.y + k, r.x + r.w + k * 0.5f, r.y + r.h + k * 1.2f, 14 + k, color(0x000000, (pal.dark ? 0.12f : 0.05f) * fade / k));
    }
    const float hx = b.x, hy = b.y - 46;
    {
        const float cx = std::clamp(hx, r.x + 20, r.x + r.w - 20), cy = std::clamp(hy, r.y + 20, r.y + r.h - 20);
        const float dl = r.x - hx, dr = hx - (r.x + r.w), dt = r.y - hy, db = hy - (r.y + r.h);
        const float most = std::max({dl, dr, dt, db});
        if (most > 4) {
            D2D1_POINT_2F a, c, tip;
            if (most == dl) a = D2D1::Point2F(r.x + 1, cy - 9), c = D2D1::Point2F(r.x + 1, cy + 9), tip = D2D1::Point2F(r.x - std::min(16.0f, dl), cy + 4);
            else if (most == dr) a = D2D1::Point2F(r.x + r.w - 1, cy - 9), c = D2D1::Point2F(r.x + r.w - 1, cy + 9), tip = D2D1::Point2F(r.x + r.w + std::min(16.0f, dr), cy + 4);
            else if (most == dt) a = D2D1::Point2F(cx - 9, r.y + 1), c = D2D1::Point2F(cx + 9, r.y + 1), tip = D2D1::Point2F(cx + 3, r.y - std::min(16.0f, dt));
            else a = D2D1::Point2F(cx - 9, r.y + r.h - 1), c = D2D1::Point2F(cx + 9, r.y + r.h - 1), tip = D2D1::Point2F(cx - 3, r.y + r.h + std::min(16.0f, db));
            ID2D1Factory* fac = factory_of(rt);
            ID2D1PathGeometry* g = nullptr;
            if (fac && SUCCEEDED(fac->CreatePathGeometry(&g))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(g->Open(&sink))) {
                    sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(tip);
                    sink->AddLine(c);
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                }
                p.br->SetColor(paper);
                rt->FillGeometry(g, p.br);
                p.br->SetColor(edge);
                rt->DrawGeometry(g, p.br, 1.4f);
                g->Release();
            }
            if (fac) fac->Release();
        }
    }
    p.fill_round(r.x, r.y, r.x + r.w, r.y + r.h, 14, paper);
    p.stroke_round(r.x + 0.7f, r.y + 0.7f, r.x + r.w - 0.7f, r.y + r.h - 0.7f, 14, edge, 1.4f);

    // the words (with "..." bouncing while the voice gets ready)
    const D2D1_COLOR_F ink = alpha(pal.text, fade), ink2 = alpha(pal.text2, fade);
    float y = r.y + 14;
    const float tw = r.w - 28 - 26;
    const float th = p.r.text_height(b.said, 0, 14.5f, DWRITE_FONT_WEIGHT_NORMAL, tw);
    p.text(b.said, BuddyRect{r.x + 14, y, tw, th + 4}, 14.5f, DWRITE_FONT_WEIGHT_NORMAL, ink, DWRITE_TEXT_ALIGNMENT_LEADING, true, 0, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    y += th + 6;
    if (!b.said_code.empty()) {
        p.fill_round(r.x + 14, y, r.x + r.w - 14, y + 26, 6, pal.dark ? color(0x000000, 0.3f * fade) : color(0x000000, 0.045f * fade));
        p.text(b.said_code, BuddyRect{r.x + 24, y, r.w - 38, 26}, 13, DWRITE_FONT_WEIGHT_NORMAL, ink, DWRITE_TEXT_ALIGNMENT_LEADING, false, 3);
        y += 32;
    }
    if (b.thinking || b.listening) {
        const D2D1_COLOR_F dot = b.listening ? color(0xFF5C8A, fade) : alpha(pal.accent, fade);
        for (int i = 0; i < 3; ++i) {
            const float jump = std::max(0.0f, std::sin(b.clock * 8 - static_cast<float>(i) * 0.9f)) * 4;
            p.fill_ellipse(r.x + r.w - 52 + static_cast<float>(i) * 10, r.y + 22 - jump, 3, 3, dot);
        }
    }

    // the close button
    {
        const BuddyRect& c = bl.close;
        if (hot_close) p.fill_ellipse(c.x + c.w / 2, c.y + c.h / 2, c.w / 2, c.h / 2, alpha(pal.text, 0.08f * fade));
        p.text(L"\xE711", c, 10, DWRITE_FONT_WEIGHT_NORMAL, ink2, DWRITE_TEXT_ALIGNMENT_CENTER, false, 2);
    }
    // the chips
    for (int i = 0; i < kBuddyChips; ++i) {
        const BuddyRect& c = bl.chips[static_cast<std::size_t>(i)];
        const bool hot = hot_chip == i;
        const D2D1_COLOR_F fill = hot ? alpha(pal.accent, (pal.dark ? 0.3f : 0.16f) * fade) : alpha(pal.text, (pal.dark ? 0.08f : 0.05f) * fade);
        p.fill_round(c.x, c.y, c.x + c.w, c.y + c.h, c.h / 2, fill);
        p.stroke_round(c.x + 0.5f, c.y + 0.5f, c.x + c.w - 0.5f, c.y + c.h - 0.5f, c.h / 2, alpha(hot ? pal.accent : pal.text, (hot ? 0.6f : 0.12f) * fade), 1);
        p.text(kChipText[i], c, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, ink, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    }
    // the mic: talk to Bit
    {
        const BuddyRect& m = bl.mic;
        const float cx = m.x + m.w / 2, cy = m.y + m.h / 2;
        if (b.listening) {
            const float pulse = std::fmod(b.clock * 1.4f, 1.0f);
            p.fill_ellipse(cx, cy, m.w / 2 + pulse * 10, m.h / 2 + pulse * 10, color(0xFF5C8A, 0.35f * (1 - pulse) * fade));
        }
        const D2D1_COLOR_F fill = b.listening ? color(0xFF5C8A, fade) : alpha(pal.accent, (hot_mic ? 0.85f : 1.0f) * fade);
        p.fill_ellipse(cx, cy, m.w / 2, m.h / 2, fill);
        p.text(b.listening ? L"\xE720" : L"\xE720", m, 17, DWRITE_FONT_WEIGHT_NORMAL, b.listening ? color(0xFFFFFF, fade) : alpha(pal.accent_text, fade),
               DWRITE_TEXT_ALIGNMENT_CENTER, false, 2);
        p.text(b.listening ? L"Listening" : L"Ask", BuddyRect{m.x - 14, m.y + m.h + 2, m.w + 28, 16}, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, ink2,
               DWRITE_TEXT_ALIGNMENT_CENTER, false);
    }
    rt->SetTransform(saved);
}

}  // namespace

void buddy_home(const Layout& l, float& x, float& y) {
    x = l.card_a.x + 54;
    y = l.card_a.bottom() - 14;
}

BuddyLayout buddy_layout(const Buddy& b, const Layout& l, Renderer& r) {
    BuddyLayout out;
    const Rect& a = l.card_a;
    out.summon = BuddyRect{a.right() - 24 - 108, a.y + 16, 108, 34};
    if (b.mode == BuddyMode::Hidden) return out;
    out.body = BuddyRect{b.x - 36, b.y - 92, 72, 94};
    float hx = 0, hy = 0;
    buddy_home(l, hx, hy);
    const float left = hx + 46, right = a.right() - 14;
    const float w = std::max(240.0f, right - left);
    const float tw = w - 28 - 26;
    float h = 14 + r.text_height(b.said.empty() ? L" " : b.said, 0, 14.5f, DWRITE_FONT_WEIGHT_NORMAL, tw) + 6;
    if (!b.said_code.empty()) h += 32;
    const float chips_top = h + 4;
    h = chips_top + 34 * 2 + 8 + 14;
    const float y = std::max(a.y + 76, hy - 6 - h);
    out.bubble = BuddyRect{left, y, w, h};
    out.close = BuddyRect{left + w - 32, y + 6, 26, 26};
    const float mic = 46;
    const float cw = (w - 28 - mic - 12 - 8) / 2;
    for (int i = 0; i < kBuddyChips; ++i)
        out.chips[static_cast<std::size_t>(i)] = BuddyRect{left + 14 + static_cast<float>(i % 2) * (cw + 8), y + chips_top + static_cast<float>(i / 2) * 42, cw, 34};
    out.mic = BuddyRect{left + w - 14 - mic, y + chips_top + 6, mic, mic};
    return out;
}

void buddy_show(Buddy& b, const Layout& l) {
    if (b.mode == BuddyMode::Here || b.mode == BuddyMode::Entering) return;
    float hx = 0, hy = 0;
    buddy_home(l, hx, hy);
    b.mode = BuddyMode::Entering;
    b.t = 0;
    b.x = hx;
    b.y = hy + 90;
    start_hop(b, hx, hy, 80);
    b.hop_height = 80;
    b.squash = b.squash_v = 0;
    b.point_t = -1;
    b.point_line = b.point_to = 0;
    b.listening = b.thinking = false;
    b.said_t = 0;
    b.blink_in = 1.2f;
    b.wave = -1;
}

void buddy_hide(Buddy& b) {
    if (b.mode == BuddyMode::Hidden || b.mode == BuddyMode::Leaving) return;
    b.mode = BuddyMode::Leaving;
    b.t = 0;
    b.listening = b.thinking = false;
    b.point_t = -1;
    b.mouth = 0;
}

void buddy_gone(Buddy& b) {
    b.mode = BuddyMode::Hidden;
    b.listening = b.thinking = false;
    b.point_t = -1;
    b.mouth = 0;
}

void buddy_say(Buddy& b, const std::wstring& text, const std::wstring& code) {
    if (text != b.said || code != b.said_code) b.said_t = 0;
    b.said = text;
    b.said_code = code;
}

void buddy_point(Buddy& b, int line, int line_to, const Layout& l) {
    if (line <= 0) {
        if (b.point_t >= 0) {
            b.point_t = -1;
            float hx = 0, hy = 0;
            buddy_home(l, hx, hy);
            start_hop(b, hx, hy, 50);
        }
        b.point_line = b.point_to = 0;
        return;
    }
    b.point_line = line;
    b.point_to = std::max(line, line_to);
    b.point_t = 0;
    b.target_shown = false;
}

void buddy_wave(Buddy& b) {
    if (b.wave < 0) b.wave = 0;
}

void buddy_mood(Buddy& b, BuddyMood mood) { b.mood = mood; }

BuddyMood buddy_shown_mood(const Buddy& b) {
    if (b.listening) return BuddyMood::Surprised;
    if (b.thinking) return BuddyMood::Curious;
    if (b.wave >= 0) return BuddyMood::Happy;
    return b.mood;
}

bool buddy_visible(const Buddy& b) { return b.mode != BuddyMode::Hidden; }

bool buddy_update(Buddy& b, float dt, const Layout& l) {
    if (b.mode == BuddyMode::Hidden) return false;
    b.t += dt;
    b.clock += dt;
    b.said_t += dt;
    // hopping
    if (b.hop < 1) {
        b.hop = std::min(1.0f, b.hop + dt / (b.mode == BuddyMode::Entering ? kEnterSeconds : kHopSeconds));
        const float k = ease_in_out(b.hop);
        b.x = lerp(b.from_x, b.to_x, k);
        b.y = lerp(b.from_y, b.to_y, k) - std::sin(kPi * b.hop) * b.hop_height;
        if (b.hop >= 1) {
            b.x = b.to_x;
            b.y = b.to_y;
            b.squash = 0.28f;   // plop
            b.squash_v = 0;
        }
    } else {
        // follow the home or the perch if it moved (the window was resized, or the lines scrolled)
        float want_x = b.to_x, want_y = b.to_y;
        if (b.point_t >= 0 && b.target_shown) perch(b, l, want_x, want_y);
        else if (b.mode != BuddyMode::Leaving && !b.stay) buddy_home(l, want_x, want_y);
        if (std::abs(want_x - b.x) > 3 || std::abs(want_y - b.y) > 3) start_hop(b, want_x, want_y, std::abs(want_x - b.x) > 60 ? 70.0f : 36.0f);
    }
    if (b.mode == BuddyMode::Entering && b.hop >= 1 && b.t > kEnterSeconds) {
        b.mode = BuddyMode::Here;
        buddy_wave(b);
    }
    // the landing wobble: a spring back to round
    const float acc = -260 * b.squash - 13 * b.squash_v;
    b.squash_v += acc * dt;
    b.squash += b.squash_v * dt;
    // blinking, now and then (sometimes twice)
    if (b.blink >= 0) {
        b.blink += dt;
        if (b.blink > kBlinkSeconds) {
            if (b.double_blink) {
                b.double_blink = false;
                b.blink = -0.0001f;
                b.blink_in = 0.12f;
            } else {
                b.blink = -1;
            }
        }
    } else {
        b.blink_in -= dt;
        if (b.blink_in <= 0) {
            b.blink = 0;
            b.blink_in = 2.0f + random01(b.seed) * 3.5f;
            b.double_blink = random01(b.seed) < 0.25f;
        }
    }
    if (b.wave >= 0) {
        b.wave += dt;
        if (b.wave > kWaveSeconds) b.wave = -1;
    }
    if (b.point_t >= 0) b.point_t += dt;
    // the eyebrows ease into the mood's shape: {lift left, lift right, tilt left, tilt right}
    {
        float want[4] = {0, 0, 0, 0};
        switch (buddy_shown_mood(b)) {
        case BuddyMood::Calm: break;
        case BuddyMood::Happy: want[0] = want[1] = -2.6f; want[2] = -7; want[3] = 7; break;
        case BuddyMood::Curious: want[0] = -4.2f; want[1] = 1.6f; want[2] = -16; want[3] = 9; break;
        case BuddyMood::Worried: want[0] = want[1] = -1.8f; want[2] = -22; want[3] = 22; break;
        case BuddyMood::Focused: want[0] = want[1] = 1.4f; want[2] = 17; want[3] = -17; break;
        case BuddyMood::Surprised: want[0] = want[1] = -4.6f; break;
        }
        const float k = 1 - std::exp(-dt * 14);
        for (int i = 0; i < 2; ++i) {
            b.brow_lift[i] += (want[i] - b.brow_lift[i]) * k;
            b.brow_tilt[i] += (want[2 + i] - b.brow_tilt[i]) * k;
        }
        if (b.brow_flick >= 0) {
            b.brow_flick += dt;
            if (b.brow_flick > 0.35f) b.brow_flick = -1;
        } else if (b.mood == BuddyMood::Calm && b.mode == BuddyMode::Here) {
            b.flick_in -= dt;
            if (b.flick_in <= 0) {
                b.brow_flick = 0;
                b.flick_in = 4 + random01(b.seed) * 5;
            }
        }
    }
    if (b.mode == BuddyMode::Leaving && b.t > kLeaveSeconds) {
        b.mode = BuddyMode::Hidden;
        return false;
    }
    return true;
}

void draw_buddy(ID2D1RenderTarget* rt, Renderer& r, const Palette& pal, const Buddy& b, const Layout& l, int hot_chip, bool hot_mic, bool hot_close) {
    if (b.mode == BuddyMode::Hidden) return;
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br))) return;
    Pen p{rt, br, r};
    const Pose pose = pose_of(b);
    const BuddyLayout bl = buddy_layout(b, l, r);
    draw_bubble(p, pal, b, bl, hot_chip, hot_mic, hot_close);
    draw_pointing(p, pal, b, pose);
    draw_bit(p, pal, b, pose);
    br->Release();
}

void draw_buddy_summon(ID2D1RenderTarget* rt, Renderer& r, const Palette& pal, const Layout& l, bool hot, float clock) {
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br))) return;
    Pen p{rt, br, r};
    const BuddyRect s = buddy_layout(Buddy{}, l, r).summon;
    p.fill_round(s.x, s.y, s.x + s.w, s.y + s.h, s.h / 2, hot ? color(pal.dark ? 0x4A4370 : 0xE6E1FF) : color(pal.dark ? 0x3A3650 : 0xF0EDFF));
    p.stroke_round(s.x + 0.5f, s.y + 0.5f, s.x + s.w - 0.5f, s.y + s.h - 0.5f, s.h / 2, color(0x7565EC, hot ? 0.7f : 0.35f), 1);
    // a tiny Bit face
    const float cx = s.x + 20, cy = s.y + s.h / 2;
    p.fill_round(cx - 12, cy - 11, cx + 12, cy + 11, 9, color(0x8B7CF6));
    p.fill_round(cx - 8, cy - 7, cx + 8, cy + 5, 5, color(0x1C2140));
    const float blink = std::fmod(clock, 4.0f) < 0.15f ? 0.2f : 1.0f;
    for (float ex : {-3.5f, 3.5f}) p.fill_round(cx + ex - 1.5f, cy - 2.5f * blink - 1, cx + ex + 1.5f, cy + 2.5f * blink - 1, 1.5f, color(0x7CF5D8));
    p.text(L"Ask Bit", BuddyRect{s.x + 38, s.y, s.w - 44, s.h}, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, pal.text, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    br->Release();
}

// ---- the see-through window over the main one

namespace {
LRESULT CALLBACK overlay_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

bool BuddyOverlay::create(HWND owner_window, ID2D1Factory* factory) {
    owner = owner_window;
    factory_ = factory;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CppSparringBuddy";
    RegisterClassExW(&wc);
    wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr,
                          wc.hInstance, nullptr);
    if (!wnd) return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    return SUCCEEDED(factory_->CreateDCRenderTarget(&props, &rt));
}

bool BuddyOverlay::resize(int width, int height, UINT) {
    if (width == w && height == h && mem) return true;
    if (mem) {
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        mem = nullptr;
    }
    w = width;
    h = height;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return false;
    old = SelectObject(mem, dib);
    return true;
}

void BuddyOverlay::present(bool show, Renderer& r, const Palette& pal, const Buddy& b, const Layout& l, UINT dpi, int hot_chip, bool hot_mic, bool hot_close) {
    if (!wnd || !rt) return;
    if (!show || !IsWindowVisible(owner) || IsIconic(owner)) {
        hide();
        return;
    }
    RECT rc{};
    GetClientRect(owner, &rc);
    if (rc.right <= 0 || rc.bottom <= 0 || !resize(rc.right, rc.bottom, dpi)) return;
    const RECT bind{0, 0, w, h};
    if (FAILED(rt->BindDC(mem, &bind))) return;
    rt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw_buddy(rt, r, pal, b, l, hot_chip, hot_mic, hot_close);
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) return;
    POINT origin{0, 0};
    ClientToScreen(owner, &origin);
    SIZE size{w, h};
    POINT zero{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, nullptr, &origin, &size, mem, &zero, 0, &blend, ULW_ALPHA);
    if (!IsWindowVisible(wnd)) ShowWindow(wnd, SW_SHOWNOACTIVATE);
}

void BuddyOverlay::hide() {
    if (wnd && IsWindowVisible(wnd)) ShowWindow(wnd, SW_HIDE);
}

void BuddyOverlay::destroy() {
    if (rt) rt->Release(), rt = nullptr;
    if (mem) {
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        mem = nullptr;
    }
    if (wnd) DestroyWindow(wnd), wnd = nullptr;
}

}  // namespace sparring
