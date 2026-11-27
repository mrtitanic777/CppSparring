#pragma once
#include <windows.h>
#include <d2d1.h>

#include <array>
#include <string>

namespace sparring {

struct Renderer;
struct Palette;
struct Layout;

// Bit, the lesson buddy: a little round robot with a screen for a face. It hops in when asked, blinks, breathes and sways while
// it waits, waves, talks with a moving mouth, and points at lines of code with an arrow and a glowing box.
enum class BuddyMode { Hidden, Entering, Here, Leaving };

// How Bit feels, shown with its eyebrows: calm, happy (raised and arched), curious (one up: "which part?"), worried (inner ends up),
// focused (inner ends down, while it points) and surprised (both high, while it listens).
enum class BuddyMood { Calm, Happy, Curious, Worried, Focused, Surprised };

constexpr int kBuddyChips = 4;   // What does this line do? / Give me a hint / Why didn't it work? / Read it to me

struct BuddyRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Buddy {
    BuddyMode mode = BuddyMode::Hidden;
    float t = 0;            // seconds in this mode
    float clock = 0;        // seconds since it came (the idle motions run on this)

    // where its feet are, in the window's DIPs; it hops between `from` and `to`
    float x = 0, y = 0;
    float from_x = 0, from_y = 0, to_x = 0, to_y = 0;
    float hop = 1;          // 0..1 along a hop (1: landed)
    float hop_height = 60;

    float squash = 0, squash_v = 0;   // the landing wobble (a spring)
    float blink_in = 2.5f;            // seconds to the next blink
    float blink = -1;                 // seconds into a blink, < 0 when the eyes are open
    bool double_blink = false;
    float wave = -1;                  // seconds into a wave, < 0 when not waving
    float mouth = 0;                  // 0..1, how open the mouth is (it follows the voice)
    bool listening = false;           // the mic is on
    bool thinking = false;            // waiting for its voice to be ready
    float look_x = 0, look_y = 0;     // what its eyes look at (DIPs)
    unsigned seed = 1;

    // eyebrows: each has a lift (DIPs, up is negative) and a tilt (degrees), easing toward the mood's shape
    BuddyMood mood = BuddyMood::Calm;
    float brow_lift[2] = {0, 0}, brow_tilt[2] = {0, 0};
    float brow_flick = -1;            // seconds into a quick raise of both brows, < 0 when none
    float flick_in = 4;               // seconds to the next one (only when calm)

    // what it says
    std::wstring said;                // the speech bubble's words
    std::wstring said_code;           // a line of code under them
    float said_t = 0;                 // seconds since the bubble changed (it pops in)
    bool why_ready = false;           // there's a run to talk about (the third chip is lit)

    // pointing at code: the lines' box in DIPs is filled in by the window every frame
    int point_line = 0, point_to = 0;
    float point_t = -1;               // seconds since it started pointing, < 0 when not
    BuddyRect target{};               // the lines pointed at, on screen
    bool target_shown = false;        // the lines are on screen (scrolled into view)
    bool stay = false;                // stands where it is put (the Name page), instead of hopping home
};

// Where its parts are, for drawing and clicking.
struct BuddyLayout {
    BuddyRect body;                    // the robot (click: it waves)
    BuddyRect bubble;
    std::array<BuddyRect, kBuddyChips> chips{};
    BuddyRect mic, close;
    BuddyRect summon;                  // the "Ask Bit" button while it is away
};

// Its home: the bottom left of the lesson's reading card.
void buddy_home(const Layout& l, float& x, float& y);
BuddyLayout buddy_layout(const Buddy& b, const Layout& l, Renderer& r);

void buddy_show(Buddy& b, const Layout& l);
void buddy_hide(Buddy& b);         // plops out
void buddy_gone(Buddy& b);         // away at once (the page changed)
void buddy_say(Buddy& b, const std::wstring& text, const std::wstring& code = L"");
void buddy_point(Buddy& b, int line, int line_to, const Layout& l);   // 0: stop pointing, and hop home
void buddy_wave(Buddy& b);
void buddy_mood(Buddy& b, BuddyMood mood);
// The mood the brows show now: listening and waiting for its voice win over the mood it was given.
BuddyMood buddy_shown_mood(const Buddy& b);
bool buddy_visible(const Buddy& b);
// Moves it on by `dt` seconds. True while it is on screen (it needs frames).
bool buddy_update(Buddy& b, float dt, const Layout& l);

// Draws Bit, its bubble and its pointing onto `rt` (the see-through layer over the window, or a screenshot).
void draw_buddy(ID2D1RenderTarget* rt, Renderer& r, const Palette& pal, const Buddy& b, const Layout& l, int hot_chip, bool hot_mic, bool hot_close);
// The "Ask Bit" button drawn on the lesson page while Bit is away.
void draw_buddy_summon(ID2D1RenderTarget* rt, Renderer& r, const Palette& pal, const Layout& l, bool hot, float clock);

// A see-through window over the main one, so Bit can draw over the code editor (a real edit box).
struct BuddyOverlay {
    HWND wnd = nullptr;
    HWND owner = nullptr;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    ID2D1DCRenderTarget* rt = nullptr;

    bool create(HWND owner_window, ID2D1Factory* factory);
    void destroy();
    // Draws and shows it over the owner's client area (or hides it when `show` is false or the owner is hidden).
    void present(bool show, Renderer& r, const Palette& pal, const Buddy& b, const Layout& l, UINT dpi, int hot_chip, bool hot_mic, bool hot_close);
    void hide();

private:
    ID2D1Factory* factory_ = nullptr;
    bool resize(int width, int height, UINT dpi);
};

}  // namespace sparring
