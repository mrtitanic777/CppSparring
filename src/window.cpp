#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "ast.hpp"
#include "compile.hpp"
#include "examples.hpp"
#include "explain.hpp"
#include "layout.hpp"
#include "mutate.hpp"
#include "lessons.hpp"
#include "palette.hpp"
#include "progress.hpp"
#include "quest.hpp"
#include "quiz.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "window.hpp"

namespace sparring {

namespace {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmSystemBackdropType = 38;
constexpr int kDwmRound = 2;
constexpr int kDwmMainWindowBackdrop = 2;   // Mica
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;

constexpr wchar_t kClassName[] = L"CppSparringWindow";
constexpr UINT kWmImportDone = WM_APP + 1;
constexpr UINT kWmRunDone = WM_APP + 2;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kReplayTimer = 2;   // while a replay plays: one step each tick
constexpr UINT kReplayStepMs = 800;
constexpr UINT kWmReplayDone = WM_APP + 3;
constexpr UINT kWmLessonDone = WM_APP + 4;
constexpr UINT kWmRunLesson = WM_APP + 5;   // F5 in the editor
constexpr float kCheerSeconds = 3.3f;
constexpr int kEditorId = 100;

COLORREF rgb(std::uint32_t c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

std::wstring crlf(const std::string& s) {
    std::wstring w = widen(s), out;
    for (wchar_t c : w) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out += c;
    }
    return out;
}

// The lesson editor: Tab types four spaces, Enter keeps the indent (and adds one after a {), F5 runs, Ctrl+A selects all.
LRESULT CALLBACK editor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    const HWND owner = reinterpret_cast<HWND>(ref);
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    "));
            return 0;
        }
        if (wp == VK_F5) {
            PostMessageW(owner, kWmRunLesson, 0, 0);
            return 0;
        }
        if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wp == VK_RETURN) {
            DWORD start = 0;
            SendMessageW(h, EM_GETSEL, reinterpret_cast<WPARAM>(&start), 0);
            const int line = static_cast<int>(SendMessageW(h, EM_LINEFROMCHAR, start, 0));
            const int line_start = static_cast<int>(SendMessageW(h, EM_LINEINDEX, line, 0));
            wchar_t buf[2048] = {};
            *reinterpret_cast<WORD*>(buf) = 2047;
            const int n = static_cast<int>(SendMessageW(h, EM_GETLINE, line, reinterpret_cast<LPARAM>(buf)));
            const std::wstring before(buf, static_cast<std::size_t>(std::clamp(static_cast<int>(start) - line_start, 0, n)));
            std::size_t indent = before.find_first_not_of(L' ');
            if (indent == std::wstring::npos) indent = before.size();
            std::wstring insert = L"\r\n" + std::wstring(indent, L' ');
            const std::size_t last = before.find_last_not_of(L' ');
            if (last != std::wstring::npos && before[last] == L'{') insert += L"    ";
            SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(insert.c_str()));
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\t' || wp == L'\r' || wp == 1) return 0;   // done in WM_KEYDOWN (1: Ctrl+A)
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, editor_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

bool system_dark() {
    DWORD value = 1, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

std::uint32_t system_accent(bool dark) {
    BYTE palette[32] = {};
    DWORD size = sizeof palette;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette", RRF_RT_REG_BINARY,
                     nullptr, palette, &size) != ERROR_SUCCESS ||
        size < 32)
        return 0;
    const BYTE* c = palette + (dark ? 1 : 4) * 4;
    return (static_cast<std::uint32_t>(c[0]) << 16) | (static_cast<std::uint32_t>(c[1]) << 8) | c[2];
}

std::string file_name(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::path(widen(path)), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// What reading a program gives: its parts, its plain-words description and its challenges.
void fill_from_nodes(Model& m, std::vector<AstNode> nodes, std::string error) {
    m.nodes = std::move(nodes);
    m.import_error = std::move(error);
    m.look = describe(m.nodes);
    m.toggles = generate_mutations(m.nodes, m.source_code);
    m.challenges.assign(m.toggles.size(), Challenge{});
    m.current = 0;
}

struct ImportResult {
    std::vector<AstNode> nodes;
    std::string error;
};

struct RunResult {
    std::size_t index = 0;
    bool has_original = false;
    CompileResult original, changed;
};

struct ReplayResult {
    unsigned id = 0;
    Replay replay;
};

struct LessonResult {
    unsigned id = 0;
    LessonCheck check;
};

struct App {
    HWND hwnd = nullptr;
    Model model;
    Layout layout;
    Palette pal;
    Renderer renderer;
    UINT dpi = 96;
    bool backdrop = false;
    bool tracking = false;
    WPARAM generation = 0;
    bool animations = true;   // Windows' "Animation effects"
    bool animating = false;
    std::chrono::steady_clock::time_point last_frame;

    unsigned replay_id = 0;

    void kick(bool always = false) {
        if ((!animations && !always) || animating) return;
        animating = true;
        last_frame = std::chrono::steady_clock::now();
        SetTimer(hwnd, kAnimTimer, 15, nullptr);
    }

    // ---- cheering
    void cheer(std::vector<Cheer> more) {
        if (more.empty()) return;
        for (Cheer& c : more) model.cheers.push_back(std::move(c));
        if (model.cheer_t < 0) {
            model.cheer_t = 0;
            if (animations && model.page != Page::Break) model.confetti = 0;
        }
        kick(true);
    }

    // ---- watching it run
    void open_replay(const std::string& source, const std::string& title) {
        if (model.nodes.empty()) return;
        model.replay_from = model.page;
        model.replay_open = true;
        model.replay_loading = true;
        model.replay_playing = false;
        model.replay_step = 0;
        model.replay_title = title;
        model.replay = Replay{};
        model.replay.source = source;
        model.page = Page::Look;
        KillTimer(hwnd, kReplayTimer);
        const unsigned id = ++replay_id;
        HWND h = hwnd;
        const std::vector<AstNode> nodes = model.nodes;
        const std::string path = model.source_path;
        const std::string input = model.program_input;
        std::thread([h, id, source, nodes, path, input] {
            auto r = std::make_unique<ReplayResult>();
            r->id = id;
            r->replay = record_replay(source, nodes, path, input);
            if (PostMessageW(h, kWmReplayDone, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
        start_page_anim();
        cheer(record_watch(model.progress));
    }

    void close_replay() {
        if (!model.replay_open) return;
        KillTimer(hwnd, kReplayTimer);
        ++replay_id;
        model.replay_open = false;
        model.replay_playing = false;
        model.replay_loading = false;
        model.highlight_line = 0;
    }

    void go_to_step(int step) {
        const int n = static_cast<int>(model.replay.steps.size());
        if (n == 0) return;
        step = std::clamp(step, 0, n - 1);
        if (step != model.replay_step && animations) {
            model.step_anim = 0;
            kick();
        }
        model.replay_step = step;
        show_line(model.replay.steps[static_cast<std::size_t>(step)].line);
        if (step == n - 1) set_playing(false);
    }

    void set_playing(bool on) {
        model.replay_playing = on;
        if (on) SetTimer(hwnd, kReplayTimer, kReplayStepMs, nullptr);
        else KillTimer(hwnd, kReplayTimer);
    }

    void seek_bar(float x) {
        const int n = static_cast<int>(model.replay.steps.size());
        if (n == 0 || layout.replay_bar.w <= 0) return;
        const float t = std::clamp((x - layout.replay_bar.x) / layout.replay_bar.w, 0.0f, 1.0f);
        go_to_step(static_cast<int>(std::lround(t * static_cast<float>(n - 1))));
    }

    // ---- the lesson editor (a real edit box over the drawing)
    HWND editor = nullptr;
    HWND input_edit = nullptr;   // the typed answers
    HFONT editor_font = nullptr;
    HBRUSH editor_brush = nullptr;
    COLORREF editor_fg = 0, editor_bg = 0;
    unsigned lesson_id = 0;

    void make_editor() {
        editor = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorId)), GetModuleHandleW(nullptr), nullptr);
        if (!editor) return;
        SetWindowSubclass(editor, editor_proc, 1, reinterpret_cast<DWORD_PTR>(hwnd));
        SendMessageW(editor, EM_SETLIMITTEXT, 0, 0);
        input_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN, 0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorId + 1)), GetModuleHandleW(nullptr), nullptr);
        model.native_editor = true;
        style_editor();
    }
    std::string edit_text(HWND h) const {
        const int n = GetWindowTextLengthW(h);
        std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(h, w.data(), n + 1);
        w.resize(static_cast<std::size_t>(n));
        std::string s = narrow(w);
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        return s;
    }
    // The answers box shows the program's answers on Look and the lesson's on a lesson.
    std::string* answers() {
        if (model.page == Page::Look && !model.replay_open && model.program_reads_input) return &model.program_input;
        if (model.page == Page::Lesson && model.lesson_reads_input) return &model.lesson_input;
        return nullptr;
    }
    void style_editor() {
        if (!editor) return;
        if (editor_font) DeleteObject(editor_font);
        editor_font = CreateFontW(-MulDiv(14, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");
        const int margin = MulDiv(10, static_cast<int>(dpi), 96);
        for (HWND h : {editor, input_edit}) {
            if (!h) continue;
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(editor_font), TRUE);
            SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
        }
        if (input_edit) InvalidateRect(input_edit, nullptr, TRUE);
        editor_bg = rgb(pal.dark ? 0x1F1F1F : 0xFAFAFA);
        editor_fg = rgb(pal.dark ? 0xE8E8E8 : 0x1B1B1B);
        if (editor_brush) DeleteObject(editor_brush);
        editor_brush = CreateSolidBrush(editor_bg);
        InvalidateRect(editor, nullptr, TRUE);
    }
    void place_editor() {
        if (!editor) return;
        auto px = [&](float v) { return static_cast<int>(std::lround(v * static_cast<float>(dpi) / 96.0f)); };
        if (const std::string* want = answers(); want && input_edit && layout.input_box.w > 0) {
            if (edit_text(input_edit) != *want) SetWindowTextW(input_edit, crlf(*want).c_str());
            const Rect& r = layout.input_box;
            SetWindowPos(input_edit, nullptr, px(r.x + 2), px(r.y + 6), px(r.w - 4), px(r.h - 10), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (input_edit) {
            ShowWindow(input_edit, SW_HIDE);
        }
        if (model.page != Page::Lesson || model.lesson < 0) {
            ShowWindow(editor, SW_HIDE);
            return;
        }
        const Rect& r = layout.editor;
        SetWindowPos(editor, nullptr, px(r.x + 2), px(r.y + 8), px(r.w - 4), px(r.h - 14), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    std::string editor_text() {
        if (!editor) return model.lesson_code;
        const int n = GetWindowTextLengthW(editor);
        std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(editor, w.data(), n + 1);
        w.resize(static_cast<std::size_t>(n));
        std::string s = narrow(w);
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        return s;
    }
    void set_editor_text(const std::string& s) {
        model.lesson_code = s;
        if (editor) SetWindowTextW(editor, crlf(s).c_str());
    }

    // ---- the write-it lessons
    void open_lesson(int index) {
        if (index < 0 || index >= static_cast<int>(lessons().size())) return;
        close_replay();
        model.lesson = index;
        model.lesson_ran = false;
        model.lesson_running = false;
        model.hint_shown = false;
        model.lesson_check = LessonCheck{};
        const Lesson& opened = lessons()[static_cast<std::size_t>(index)];
        model.lesson_reads_input = lesson_reads_input(opened);
        model.lesson_input = !opened.input.empty() ? opened.input : model.lesson_reads_input ? "7\n" : "";
        model.page = Page::Lesson;
        set_editor_text(lessons()[static_cast<std::size_t>(index)].starter);
        relayout();
        if (editor) SetFocus(editor);
        start_page_anim();
    }
    void run_lesson() {
        if (model.page != Page::Lesson || model.lesson < 0 || model.lesson_running) return;
        sync_answers();
        model.lesson_code = editor_text();
        model.lesson_running = true;
        const unsigned id = ++lesson_id;
        HWND h = hwnd;
        const Lesson lesson = lessons()[static_cast<std::size_t>(model.lesson)];
        const std::string code = model.lesson_code;
        const std::string input = model.lesson_input;
        std::thread([h, id, lesson, code, input] {
            auto r = std::make_unique<LessonResult>();
            r->id = id;
            r->check = check_lesson(lesson, code, input);
            if (PostMessageW(h, kWmLessonDone, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
        invalidate();
    }

    // ---- the Daily Quest
    QuestStep* step() {
        Quest& q = model.quest;
        return q.active && q.at < static_cast<int>(q.steps.size()) ? &q.steps[static_cast<std::size_t>(q.at)] : nullptr;
    }
    void start_quest() {
        close_replay();
        model.quest = make_quest(model.progress);
        open_step();
    }
    void open_step() {
        Quest& q = model.quest;
        if (q.at >= static_cast<int>(q.steps.size()) || (q.boss && q.boss_hp <= 0)) {
            finish_quest();
            return;
        }
        QuestStep& s = q.steps[static_cast<std::size_t>(q.at)];
        q.time_left = kBossSeconds;
        switch (s.kind) {
        case StepKind::Quiz:
            model.quiz = s.quiz;
            model.quiz_choice = -1;
            model.page = Page::Quiz;
            start_page_anim();
            break;
        case StepKind::Write:
            open_lesson(s.lesson);
            break;
        case StepKind::Guess: {
            const Example& e = examples()[static_cast<std::size_t>(s.example)];
            if (model.source_name == e.file && !model.importing && !model.challenges.empty()) {
                prepare_guess();
            } else {
                load_file(write_example(e));
                model.page = Page::Break;
            }
            start_page_anim();
            break;
        }
        }
        if (q.boss && q.timer) kick(true);
        relayout();
        invalidate();
    }
    // The program of a Guess step has been read: ask one of its challenges.
    void prepare_guess() {
        QuestStep* s = step();
        if (!s || s->kind != StepKind::Guess) return;
        if (s->challenge < 0) s->challenge = choose_challenge(model.progress, model.quest, model.source_name, model.toggles);
        if (s->challenge < 0) {
            skip_step();
            return;
        }
        model.current = s->challenge;
        model.challenges[static_cast<std::size_t>(s->challenge)] = Challenge{};
        model.page = Page::Break;
        model.quest.time_left = kBossSeconds;
    }
    void advance_quest() {
        ++model.quest.at;
        open_step();
    }
    void skip_step() {
        if (QuestStep* s = step()) {
            s->done = s->skipped = true;
            model.quest.combo = 0;
        }
        advance_quest();
    }
    void finish_quest() {
        Quest& q = model.quest;
        q.finished = true;
        const bool beaten = q.boss && q.boss_hp <= 0;
        const int before = model.progress.xp;
        cheer(record_quest(model.progress, q.boss, beaten, q.chest_xp));
        q.xp += model.progress.xp - before - q.chest_xp;
        model.page = Page::QuestDone;
        start_page_anim();
        if (animations) model.confetti = 0;
        relayout();
    }

    // The Bug Boss timer counts down only while a guess is waiting.
    bool boss_clock_running() const {
        const Quest& q = model.quest;
        if (!q.active || !q.boss || !q.timer || q.finished || model.page != Page::Break || model.running || model.importing) return false;
        const int at = q.at;
        if (at >= static_cast<int>(q.steps.size())) return false;
        const QuestStep& s = q.steps[static_cast<std::size_t>(at)];
        return s.challenge >= 0 && s.challenge < static_cast<int>(model.challenges.size()) &&
               model.challenges[static_cast<std::size_t>(s.challenge)].guess < 0;
    }
    void end_quest() {
        model.quest = Quest{};
        model.page = Page::Home;
        start_page_anim();
    }
    void start_page_anim() {
        if (!animations) return;
        model.page_anim = 0;
        model.confetti = -1;
        kick();
    }
    void on_frame() {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.05f, std::chrono::duration<float>(now - last_frame).count());
        last_frame = now;
        model.page_anim = std::min(1.0f, model.page_anim + dt / 0.22f);
        model.result_anim = std::min(1.0f, model.result_anim + dt / 0.32f);
        model.hover_anim = std::min(1.0f, model.hover_anim + dt / 0.12f);
        model.step_anim = std::min(1.0f, model.step_anim + dt / 0.5f);
        if (model.confetti >= 0) {
            model.confetti += dt;
            if (model.confetti > 1.7f) model.confetti = -1;
        }
        if (model.cheer_t >= 0) {
            model.cheer_t += dt;
            if (model.cheer_t > kCheerSeconds) {
                if (!model.cheers.empty()) model.cheers.erase(model.cheers.begin());
                model.cheer_t = model.cheers.empty() ? -1.0f : 0.0f;
            }
        }
        const bool clock = boss_clock_running();
        if (clock) {
            model.quest.time_left -= dt;
            if (model.quest.time_left <= 0) skip_step();   // time's up: no penalty, just on to the next one
        }
        if (model.page_anim >= 1 && model.result_anim >= 1 && model.hover_anim >= 1 && model.step_anim >= 1 && model.confetti < 0 &&
            model.cheer_t < 0 && !clock) {
            KillTimer(hwnd, kAnimTimer);
            animating = false;
        }
        invalidate();
    }

    float dip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(dpi); }
    void invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }

    void relayout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        layout = compute_layout(model, dip(rc.right), dip(rc.bottom));
        place_editor();
    }

    void apply_theme() {
        const bool dark = system_dark();
        pal = make_palette(dark, system_accent(dark));
        const BOOL d = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &d, sizeof d);
        style_editor();
    }

    void on_create() {
        dpi = GetDpiForWindow(hwnd);
        BOOL on = TRUE;
        animations = !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on != FALSE;
        const int corner = kDwmRound;
        DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof corner);
        const int type = kDwmMainWindowBackdrop;
        backdrop = SUCCEEDED(DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &type, sizeof type));
        if (backdrop) {
            const MARGINS all{-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(hwnd, &all);
            const DWORD none = kDwmColorNone;
            DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &none, sizeof none);
        }
        apply_theme();
        make_editor();
        DragAcceptFiles(hwnd, TRUE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void set_hot(Control c, int i) {
        if (c == model.hot && i == model.hot_index) return;
        model.hot = c;
        model.hot_index = i;
        if (animations && c != Control::None) {
            model.hover_anim = 0;
            kick();
        }
        invalidate();
    }

    Control hit(float x, float y, int& index) const {
        index = -1;
        if (layout.caption_close.contains(x, y)) return Control::CaptionClose;
        if (layout.caption_maximize.contains(x, y)) return Control::CaptionMaximize;
        if (layout.caption_minimize.contains(x, y)) return Control::CaptionMinimize;
        if (layout.nav_home.contains(x, y)) return Control::NavHome;
        if (layout.nav_practice.contains(x, y)) return Control::NavPractice;
        if (layout.nav_badges.contains(x, y)) return Control::NavBadges;
        if (layout.nav_learn.contains(x, y)) return Control::NavLearn;
        if (layout.quest_skip.contains(x, y)) return Control::QuestSkip;
        if (layout.quest_quit.contains(x, y)) return Control::QuestQuit;
        if (layout.session_back.contains(x, y)) return Control::SessionBack;
        if (layout.step_look.contains(x, y)) return Control::StepLook;
        if (layout.step_break.contains(x, y)) return Control::StepBreak;
        auto in = [&](const auto& rects, Control c) {
            for (std::size_t i = 0; i < rects.size(); ++i)
                if (rects[i].contains(x, y)) {
                    index = static_cast<int>(i);
                    return c;
                }
            return Control::None;
        };
        switch (model.page) {
        case Page::Badges: break;
        case Page::Lessons:
            if (layout.lessons_area.contains(x, y)) {
                const Control c = in(layout.lesson_tiles, Control::LessonTile);
                return c != Control::None ? c : Control::LessonList;
            }
            break;
        case Page::Lesson:
            if (layout.run_button.contains(x, y)) return Control::LessonRun;
            if (layout.hint_button.contains(x, y)) return Control::LessonHint;
            if (layout.reset_button.contains(x, y)) return Control::LessonReset;
            if (layout.lesson_next.contains(x, y)) return Control::LessonNext;
            if (layout.editor.contains(x, y)) return Control::Editor;
            break;
        case Page::Quiz:
            if (model.quiz_choice >= 0 && layout.quiz_next.contains(x, y)) return Control::QuizNext;
            if (model.quiz_choice < 0) return in(layout.quiz_answers, Control::QuizAnswer);
            break;
        case Page::QuestDone:
            if (layout.chest.contains(x, y) && !model.quest.chest_open) return Control::ChestOpen;
            if (layout.done_home.contains(x, y)) return Control::QuestHome;
            if (layout.done_again.contains(x, y)) return Control::QuestAgain;
            break;
        case Page::Home:
            if (layout.hero_button.contains(x, y)) return Control::HeroQuest;
            if (layout.doors[0].contains(x, y)) return Control::DoorPractice;
            if (layout.doors[1].contains(x, y)) return Control::DoorLessons;
            if (layout.doors[2].contains(x, y)) return Control::DoorBadges;
            break;
        case Page::Practice:
            for (std::size_t i = 0; i < layout.level_tabs.size(); ++i)
                if (layout.level_tabs[i].contains(x, y)) {
                    index = static_cast<int>(i);
                    return Control::LevelTab;
                }
            if (!level_open(model.progress, model.pick_level)) {
                if (layout.open_button.contains(x, y)) return Control::OpenFile;
                break;
            }
            for (std::size_t i = 0; i < layout.examples.size(); ++i)
                if (layout.examples[i].contains(x, y)) {
                    index = static_cast<int>(i);
                    return Control::Example;
                }
            if (layout.open_button.contains(x, y)) return Control::OpenFile;
            break;
        case Page::Look:
            if (model.replay_open) {
                if (layout.code_view.contains(x, y)) return Control::CodeView;
                if (layout.replay_stop.contains(x, y)) return Control::ReplayStop;
                if (model.replay_loading || model.replay.steps.empty()) break;
                if (layout.replay_back.contains(x, y)) return Control::ReplayBack;
                if (layout.replay_play.contains(x, y)) return Control::ReplayPlay;
                if (layout.replay_next.contains(x, y)) return Control::ReplayNext;
                if (Rect{layout.replay_bar.x - 8, layout.replay_bar.y - 6, layout.replay_bar.w + 16, layout.replay_bar.h + 12}.contains(x, y))
                    return Control::ReplayBar;
                break;
            }
            if (layout.watch_button.contains(x, y)) return Control::Watch;
            if (layout.next_step.contains(x, y)) return Control::NextStep;
            if (layout.code_view.contains(x, y)) return Control::CodeView;
            for (std::size_t i = 0; i < layout.rows.size(); ++i)
                if (layout.rows[i].contains(x, y)) {
                    index = static_cast<int>(i);
                    return Control::LookRow;
                }
            if (layout.look_list.contains(x, y)) return Control::LookList;
            break;
        case Page::Break:
            if (layout.boss_timer.contains(x, y)) return Control::BossTimer;
            if (model.challenges.empty()) break;
            if (layout.prev_button.contains(x, y)) return Control::PrevChallenge;
            if (layout.next_button.contains(x, y)) return Control::NextChallenge;
            if (layout.next_challenge.contains(x, y) && current_challenge().ran) return Control::NextAfterResult;
            if (layout.watch_changed.contains(x, y) && current_challenge().ran) return Control::WatchChanged;
            for (int i = 0; i < 4; ++i)
                if (layout.guesses[static_cast<std::size_t>(i)].contains(x, y)) {
                    index = i;
                    return Control::Guess;
                }
            break;
        }
        return Control::None;
    }

    Challenge& current_challenge() { return model.challenges[static_cast<std::size_t>(model.current)]; }
    const Challenge& current_challenge() const { return model.challenges[static_cast<std::size_t>(model.current)]; }

    void step_challenge(int delta) {
        const int n = static_cast<int>(model.challenges.size());
        if (n == 0 || model.running) return;
        model.current = ((model.current + delta) % n + n) % n;
    }

    void show_line(int line) {
        model.highlight_line = line;
        const int visible = std::max(1, static_cast<int>((layout.code_view.h - 16) / kCodeLineHeight));
        if (line - 1 < model.code_scroll || line > model.code_scroll + visible) model.code_scroll = std::max(0, line - 1 - visible / 3);
    }

    // What is in the answers box now (Windows does not always say when a multi-line box changes).
    void sync_answers() {
        if (std::string* field = answers(); field && input_edit && (GetWindowLongPtrW(input_edit, GWL_STYLE) & WS_VISIBLE)) *field = edit_text(input_edit);
    }

    void click(Control c, int i) {
        sync_answers();
        const Page was_page = model.page;
        const int was_current = model.current;
        act(c, i);
        if (model.page != was_page || model.current != was_current) start_page_anim();
        relayout();
        invalidate();
    }

    void act(Control c, int i) {
        const bool loaded = !model.source_path.empty();
        switch (c) {
        case Control::CaptionClose: PostMessageW(hwnd, WM_CLOSE, 0, 0); return;
        case Control::CaptionMaximize: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return;
        case Control::CaptionMinimize: ShowWindow(hwnd, SW_MINIMIZE); return;
        case Control::NavHome:
            close_replay();
            model.page = Page::Home;
            break;
        case Control::NavPractice:
        case Control::DoorPractice:
            close_replay();
            model.page = Page::Practice;
            break;
        case Control::NavBadges:
        case Control::DoorBadges:
            close_replay();
            model.page = Page::Badges;
            break;
        case Control::NavLearn:
        case Control::DoorLessons:
            close_replay();
            model.page = Page::Lessons;
            break;
        case Control::SessionBack:
            close_replay();
            model.page = model.page == Page::Lesson ? Page::Lessons : Page::Practice;
            break;
        case Control::StepLook:
            if (!loaded) break;
            close_replay();
            model.page = Page::Look;
            break;
        case Control::StepBreak:
            if (!loaded) break;
            close_replay();
            model.page = Page::Break;
            break;
        case Control::HeroQuest:
            if (model.quest.active && !model.quest.finished) open_step();
            else start_quest();
            return;
        case Control::QuestSkip: skip_step(); return;
        case Control::QuestQuit: end_quest(); break;
        case Control::BossTimer:
            model.quest.timer = !model.quest.timer;
            model.quest.time_left = kBossSeconds;
            kick(true);
            break;
        case Control::LessonTile:
            if (i >= 0 && i < static_cast<int>(layout.lesson_items.size())) open_lesson(layout.lesson_items[static_cast<std::size_t>(i)]);
            return;
        case Control::LessonRun: run_lesson(); break;
        case Control::LessonHint: model.hint_shown = !model.hint_shown; break;
        case Control::LessonReset:
            if (model.lesson >= 0) set_editor_text(lessons()[static_cast<std::size_t>(model.lesson)].starter);
            model.lesson_ran = false;
            break;
        case Control::LessonNext:
            if (!model.lesson_ran || !model.lesson_check.passed) break;
            if (model.quest.active && !model.quest.finished) advance_quest();
            else {
                const std::vector<int> order = lesson_order();
                const auto at = std::find(order.begin(), order.end(), model.lesson);
                if (at != order.end() && at + 1 != order.end()) open_lesson(*(at + 1));
                else model.page = Page::Lessons;
            }
            return;
        case Control::QuizAnswer:
            if (model.quiz_choice < 0 && i >= 0 && model.quiz >= 0) {
                model.quiz_choice = i;
                const bool right = answer_order(static_cast<std::size_t>(model.quiz))[static_cast<std::size_t>(i)] == 0;
                const int before = model.progress.xp;
                cheer(record_quiz(model.progress, quiz_cards()[static_cast<std::size_t>(model.quiz)].id, right));
                if (QuestStep* s = step(); s && s->kind == StepKind::Quiz) {
                    s->done = true;
                    s->right = right;
                    model.quest.xp += model.progress.xp - before;
                }
                if (right && animations) {
                    model.confetti = 0;
                    kick();
                }
            }
            break;
        case Control::QuizNext:
            if (model.quest.active && !model.quest.finished) advance_quest();
            else model.page = Page::Home;
            return;
        case Control::ChestOpen:
            model.quest.chest_open = true;
            if (animations) {
                model.confetti = 0;
                kick();
            }
            break;
        case Control::QuestHome: end_quest(); break;
        case Control::QuestAgain: start_quest(); return;
        case Control::LevelTab:
            if (i >= 0 && i < static_cast<int>(levels().size())) {
                model.pick_level = levels()[static_cast<std::size_t>(i)].number;
                start_page_anim();
            }
            break;
        case Control::Example:
            if (i >= 0 && i < static_cast<int>(layout.example_items.size())) {
                const Example& e = examples()[layout.example_items[static_cast<std::size_t>(i)]];
                if (model.source_name == e.file && !model.importing && !model.nodes.empty()) model.page = Page::Look;   // already open: carry on
                else load_file(write_example(e));
            }
            return;
        case Control::Watch: open_replay(model.source_code, "Watching it run"); break;
        case Control::WatchChanged:
            if (!model.challenges.empty())
                open_replay(apply_mutation(model.source_code, model.toggles[static_cast<std::size_t>(model.current)]), "Watching your change run");
            break;
        case Control::ReplayBack:
            set_playing(false);
            go_to_step(model.replay_step - 1);
            break;
        case Control::ReplayNext:
            set_playing(false);
            go_to_step(model.replay_step + 1);
            break;
        case Control::ReplayPlay:
            if (!model.replay_playing && model.replay_step + 1 >= static_cast<int>(model.replay.steps.size())) go_to_step(0);
            set_playing(!model.replay_playing);
            break;
        case Control::ReplayStop: {
            const Page back = model.replay_from;
            close_replay();
            model.page = back;
            break;
        }
        case Control::OpenFile: browse(); return;
        case Control::LookRow:
            if (i >= 0 && layout.look_first + i < static_cast<int>(model.look.size()))
                show_line(model.look[static_cast<std::size_t>(layout.look_first + i)].line);
            break;
        case Control::NextStep:
            if (!model.importing && !model.toggles.empty()) model.page = Page::Break;
            break;
        case Control::PrevChallenge: step_challenge(-1); break;
        case Control::NextChallenge: step_challenge(+1); break;
        case Control::NextAfterResult:
            if (model.quest.active && !model.quest.finished) {
                advance_quest();
                return;
            }
            step_challenge(+1);
            break;
        case Control::Guess:
            if (!model.running && current_challenge().guess < 0) {
                current_challenge().guess = i;
                run_challenge();
            }
            break;
        default: break;
        }
        relayout();
        invalidate();
    }

    void load_file(const std::string& path) {
        std::string code;
        if (!read_file(path, code)) {
            model.import_error = "Could not open " + file_name(path);
            invalidate();
            return;
        }
        close_replay();
        model.source_path = path;
        model.source_name = file_name(path);
        model.source_code = std::move(code);
        model.program_reads_input = reads_input(model.source_code);
        model.program_input.clear();
        for (const Example& e : examples())
            if (model.source_name == e.file && e.input) model.program_input = e.input;
        model.code_scroll = 0;
        model.highlight_line = 0;
        model.look_scroll = 0;
        fill_from_nodes(model, {}, {});
        model.original_ready = false;
        model.running = false;
        model.importing = true;
        model.page = Page::Look;
        start_page_anim();

        const WPARAM gen = ++generation;
        HWND h = hwnd;
        std::thread([h, gen, path] {
            auto r = std::make_unique<ImportResult>();
            r->nodes = parse_ast(path, &r->error);
            if (PostMessageW(h, kWmImportDone, gen, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
        relayout();
        invalidate();
    }

    void run_challenge() {
        if (model.running || model.challenges.empty()) return;
        model.running = true;
        const WPARAM gen = generation;
        HWND h = hwnd;
        const std::size_t index = static_cast<std::size_t>(model.current);
        const bool need_original = !model.original_ready;
        const std::string path = model.source_path;
        const std::string changed = apply_mutation(model.source_code, model.toggles[index]);
        const std::string input = model.program_input;
        std::thread([h, gen, index, need_original, path, changed, input] {
            auto r = std::make_unique<RunResult>();
            r->index = index;
            r->has_original = need_original;
            if (need_original) r->original = compile_and_run(path, input);
            r->changed = compile_and_run_string(changed, path, input);
            if (PostMessageW(h, kWmRunDone, gen, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
    }

    void browse() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
        const COMDLG_FILTERSPEC filters[] = {{L"C++ programs", L"*.cpp;*.cxx;*.cc;*.c++"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(L"Open your C++ program");
        if (FAILED(dlg->Show(hwnd))) return;
        ComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return;
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            load_file(narrow(p));
            CoTaskMemFree(p);
        }
    }

    int code_line_count() const { return static_cast<int>(std::count(model.source_code.begin(), model.source_code.end(), '\n')) + 1; }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
};

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        on_create();
        return 0;

    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            // the system keeps its borders; the caption it would take is ours (the client area reaches the top)
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const LONG top = params->rgrc[0].top;
            const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
            if (result != 0) return result;
            params->rgrc[0].top = top;
            if (IsZoomed(hwnd)) params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        const LRESULT system = DefWindowProcW(hwnd, msg, wp, lp);
        if (system != HTCLIENT) return system;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int border = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi) - 2;
        if (!IsZoomed(hwnd) && p.y < border) {
            if (p.x < border) return HTTOPLEFT;
            if (p.x >= rc.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
        const float x = dip(p.x), y = dip(p.y);
        if (layout.caption_close.contains(x, y)) return HTCLOSE;
        if (layout.caption_maximize.contains(x, y)) return HTMAXBUTTON;
        if (layout.caption_minimize.contains(x, y)) return HTMINBUTTON;
        if (y < kTitleBarHeight) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_NCMOUSEMOVE: {
        Control c = Control::None;
        if (wp == HTCLOSE) c = Control::CaptionClose;
        else if (wp == HTMAXBUTTON) c = Control::CaptionMaximize;
        else if (wp == HTMINBUTTON) c = Control::CaptionMinimize;
        set_hot(c, -1);
        if (!tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            tracking = TrackMouseEvent(&t) != FALSE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_NCMOUSELEAVE:
        tracking = false;
        if (GetCapture() != hwnd) set_hot(Control::None, -1);
        return 0;

    case WM_NCLBUTTONDOWN:
        if (wp == HTCLOSE || wp == HTMAXBUTTON || wp == HTMINBUTTON) {
            model.pressed = wp == HTCLOSE ? Control::CaptionClose : wp == HTMAXBUTTON ? Control::CaptionMaximize : Control::CaptionMinimize;
            model.pressed_index = -1;
            SetCapture(hwnd);
            invalidate();
            return 0;
        }
        break;

    case WM_NCLBUTTONUP:
        if (wp == HTCLOSE || wp == HTMAXBUTTON || wp == HTMINBUTTON) return 0;
        break;

    case WM_MOUSEMOVE: {
        if (model.pressed == Control::ReplayBar && (wp & MK_LBUTTON)) {
            seek_bar(dip(GET_X_LPARAM(lp)));
            invalidate();
            return 0;
        }
        int i = -1;
        const Control c = hit(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)), i);
        set_hot(c, i);
        if (!tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const Control c = model.hot;
            const bool pointer = c != Control::None && c != Control::CodeView && c != Control::LookList && c != Control::CaptionClose &&
                                 c != Control::CaptionMaximize && c != Control::CaptionMinimize;
            SetCursor(LoadCursorW(nullptr, pointer ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_MOUSELEAVE:
        tracking = false;
        if (GetCapture() != hwnd) set_hot(Control::None, -1);
        return 0;

    case WM_LBUTTONDOWN: {
        int i = -1;
        model.pressed = hit(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)), i);
        model.pressed_index = i;
        if (model.pressed != Control::None) SetCapture(hwnd);
        if (model.pressed == Control::ReplayBar) {
            set_playing(false);
            seek_bar(dip(GET_X_LPARAM(lp)));
        }
        invalidate();
        return 0;
    }

    case WM_LBUTTONUP: {
        const Control was = model.pressed;
        const int was_index = model.pressed_index;
        model.pressed = Control::None;
        model.pressed_index = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        int i = -1;
        const Control at = hit(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)), i);
        set_hot(at, i);
        if (was != Control::None && was == at && i == was_index) click(was, i);
        invalidate();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        const float x = dip(p.x), y = dip(p.y);
        const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (model.page == Page::Look && layout.code_view.contains(x, y)) {
            const int visible = std::max(1, static_cast<int>((layout.code_view.h - 16) / kCodeLineHeight));
            model.code_scroll = std::clamp(model.code_scroll - steps * 3, 0, std::max(0, code_line_count() - visible));
        } else if (model.page == Page::Lessons && layout.lessons_area.contains(x, y)) {
            const int most = std::max(0, static_cast<int>(layout.lessons_height - layout.lessons_area.h) + 20);
            model.lessons_scroll = std::clamp(model.lessons_scroll - steps * 66, 0, most);
            relayout();
        } else if (model.page == Page::Look && layout.look_list.contains(x, y)) {
            model.look_scroll = std::clamp(model.look_scroll - steps, 0, std::max(0, static_cast<int>(model.look.size()) - 1));
            relayout();
            model.look_scroll = layout.look_first;
        } else {
            break;
        }
        invalidate();
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_F5 && model.page == Page::Lesson) {
            run_lesson();
            return 0;
        }
        if (model.replay_open && !model.replay.steps.empty() && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_SPACE)) {
            if (wp == VK_SPACE) act(Control::ReplayPlay, -1);
            else {
                set_playing(false);
                go_to_step(model.replay_step + (wp == VK_LEFT ? -1 : 1));
            }
            invalidate();
            return 0;
        }
        if (model.page == Page::Break && (wp == VK_LEFT || wp == VK_RIGHT)) {
            step_challenge(wp == VK_LEFT ? -1 : +1);
            invalidate();
            return 0;
        }
        break;

    case WM_SIZE:
        model.maximized = IsZoomed(hwnd) != FALSE;
        renderer.resize(LOWORD(lp), HIWORD(lp));
        relayout();
        invalidate();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = MulDiv(900, static_cast<int>(dpi), 96);
        mm->ptMinTrackSize.y = MulDiv(680, static_cast<int>(dpi), 96);
        return 0;
    }

    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        renderer.set_dpi(dpi);
        style_editor();
        const RECT* rc = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        relayout();
        invalidate();
        return 0;
    }

    case WM_ACTIVATE:
        model.active = LOWORD(wp) != WA_INACTIVE;
        invalidate();
        break;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        apply_theme();
        invalidate();
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (renderer.ensure_target(hwnd, dpi) && renderer.paint(renderer.target.Get(), model, layout, pal, backdrop) == D2DERR_RECREATE_TARGET) {
            renderer.brush.Reset();
            renderer.target.Reset();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) load_file(narrow(path));
        DragFinish(drop);
        return 0;
    }

    case kWmImportDone: {
        std::unique_ptr<ImportResult> r(reinterpret_cast<ImportResult*>(lp));
        if (wp != generation) return 0;
        model.importing = false;
        fill_from_nodes(model, std::move(r->nodes), std::move(r->error));
        if (!model.challenges.empty()) {
            model.progress.totals[model.source_name] = static_cast<int>(model.challenges.size());
            save_progress(model.progress);
        }
        if (QuestStep* s = step(); s && s->kind == StepKind::Guess && !model.quest.finished) prepare_guess();
        relayout();
        invalidate();
        return 0;
    }

    case kWmLessonDone: {
        std::unique_ptr<LessonResult> r(reinterpret_cast<LessonResult*>(lp));
        if (r->id != lesson_id || model.lesson < 0) return 0;
        model.lesson_running = false;
        model.lesson_ran = true;
        model.lesson_check = std::move(r->check);
        const bool passed = model.lesson_check.passed;
        const int before = model.progress.xp;
        cheer(record_lesson(model.progress, lessons()[static_cast<std::size_t>(model.lesson)].id, passed));
        if (QuestStep* s = step(); s && s->kind == StepKind::Write && s->lesson == model.lesson && passed) {
            s->done = s->right = true;
            model.quest.xp += model.progress.xp - before;
        }
        if (passed && animations) {
            model.confetti = 0;
            kick();
        }
        invalidate();
        return 0;
    }

    case kWmRunLesson:
        run_lesson();
        return 0;

    case WM_COMMAND:
        if (input_edit && reinterpret_cast<HWND>(lp) == input_edit && HIWORD(wp) == EN_CHANGE) {
            if (std::string* field = answers()) *field = edit_text(input_edit);
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT:
        if ((reinterpret_cast<HWND>(lp) == editor || reinterpret_cast<HWND>(lp) == input_edit) && editor_brush) {
            SetTextColor(reinterpret_cast<HDC>(wp), editor_fg);
            SetBkColor(reinterpret_cast<HDC>(wp), editor_bg);
            return reinterpret_cast<LRESULT>(editor_brush);
        }
        break;

    case kWmReplayDone: {
        std::unique_ptr<ReplayResult> r(reinterpret_cast<ReplayResult*>(lp));
        if (r->id != replay_id || !model.replay_open) return 0;
        model.replay_loading = false;
        model.replay = std::move(r->replay);
        model.replay_step = 0;
        if (!model.replay.steps.empty()) {
            show_line(model.replay.steps.front().line);
            set_playing(true);
        }
        relayout();
        invalidate();
        return 0;
    }

    case kWmRunDone: {
        std::unique_ptr<RunResult> r(reinterpret_cast<RunResult*>(lp));
        if (wp != generation) return 0;
        model.running = false;
        if (r->has_original) {
            model.original = std::move(r->original);
            model.original_ready = true;
        }
        if (r->index < model.challenges.size()) {
            Challenge& c = model.challenges[r->index];
            c.ran = true;
            c.result = std::move(r->changed);
            c.truth = model.original.compiled ? static_cast<int>(classify(model.original, c.result)) : -1;
            if (animations && r->index == static_cast<std::size_t>(model.current)) {
                model.result_anim = 0;
                if (c.truth >= 0 && c.guess == c.truth) model.confetti = 0;
                kick();
            }
            if (c.truth >= 0) {
                const bool right = c.guess == c.truth;
                QuestStep* s = step();
                const bool in_quest = s && s->kind == StepKind::Guess && s->challenge == static_cast<int>(r->index);
                int bonus = 0;
                Quest& q = model.quest;
                if (in_quest && q.boss) {
                    if (right) {
                        ++q.combo;
                        q.best_combo = std::max(q.best_combo, q.combo);
                        q.boss_hp = std::max(0, q.boss_hp - 1);
                        bonus = combo_bonus(q.combo);
                    } else {
                        q.combo = 0;
                    }
                }
                const int before = model.progress.xp;
                cheer(record_guess(model.progress, model.source_name, model.toggles[r->index], right, static_cast<Outcome>(c.truth), bonus));
                if (in_quest) {
                    s->done = true;
                    s->right = right;
                    q.xp += model.progress.xp - before;
                }
            } else if (QuestStep* s = step(); s && s->kind == StepKind::Guess) {
                s->done = s->skipped = true;   // the program itself doesn't build: nothing to guess
            }
        }
        relayout();
        invalidate();
        return 0;
    }

    case WM_TIMER:
        if (wp == kAnimTimer) {
            on_frame();
            return 0;
        }
        if (wp == kReplayTimer) {
            if (model.replay_open && model.replay_playing) go_to_step(model.replay_step + 1);
            else KillTimer(hwnd, kReplayTimer);
            invalidate();
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
    return app->handle(msg, wp, lp);
}

int render_png(const ShotOptions& o) {
    const std::wstring& out_png = o.out_png;
    const std::wstring& file = o.file;
    const std::wstring& page = o.page;
    const int guess = o.guess;
    const bool dark = o.dark;
    Renderer renderer;
    if (!renderer.init()) return 1;

    Model m;
    m.progress = load_progress();
    if (o.demo) {
        Progress p;
        p.totals = {{"robot_age.cpp", 3}, {"snack_count.cpp", 2}, {"piggy_bank.cpp", 4}};
        p.right = {"robot_age.cpp|a@1", "robot_age.cpp|b@2", "robot_age.cpp|c@3", "snack_count.cpp|a@1", "piggy_bank.cpp|a@1"};
        p.badges = {"first", "right", "streak3", "ref", "watch", "perfect", "level2"};
        p.streak = 3;
        p.best_streak = 4;
        p.guesses = 7;
        p.correct = 5;
        m.progress = p;
        m.cheers.push_back(Cheer{"Badge: On a roll", "Guess right 3 times in a row.", L"\xE945", true});
        m.cheer_t = 1.0f;
    }
    m.pick_level = o.level > 0 ? o.level : highest_open_level(m.progress);
    if (!file.empty()) {
        std::string path = narrow(file);
        if (path.rfind("example:", 0) == 0) {
            const std::size_t i = static_cast<std::size_t>(std::stoi(path.substr(8)));
            if (i < examples().size()) path = write_example(examples()[i]);
        }
        read_file(path, m.source_code);
        m.program_reads_input = reads_input(m.source_code);
        for (const Example& e : examples())
            if (file_name(path) == e.file && e.input) m.program_input = e.input;
        m.source_path = path;
        m.source_name = file_name(path);
        std::string error;
        std::vector<AstNode> nodes = parse_ast(path, &error);
        fill_from_nodes(m, std::move(nodes), std::move(error));
        if (!m.look.empty()) m.highlight_line = m.look.front().line;
        if (guess >= 0 && !m.challenges.empty()) {
            m.original = compile_and_run(path, m.program_input);
            m.original_ready = true;
            Challenge& c = m.challenges[0];
            c.guess = guess;
            c.result = compile_and_run_string(apply_mutation(m.source_code, m.toggles[0]), path, m.program_input);
            c.ran = true;
            c.truth = m.original.compiled ? static_cast<int>(classify(m.original, c.result)) : -1;
            if (c.truth == guess) m.confetti = 0.35f;   // show the celebration mid-flight
        }
        if (o.replay_step >= 0) {
            const bool changed = guess >= 0 && !m.toggles.empty();
            m.replay = record_replay(changed ? apply_mutation(m.source_code, m.toggles[0]) : m.source_code, m.nodes, path, m.program_input);
            m.replay_open = true;
            m.replay_from = changed ? Page::Break : Page::Look;
            m.replay_title = changed ? "Watching your change run" : "Watching it run";
            m.replay_step = std::min(o.replay_step, std::max(0, static_cast<int>(m.replay.steps.size()) - 1));
            if (!m.replay.steps.empty()) m.highlight_line = m.replay.steps[static_cast<std::size_t>(m.replay_step)].line;
        }
    }
    m.page = page == L"look"        ? Page::Look
             : page == L"break"     ? Page::Break
             : page == L"badges"    ? Page::Badges
             : page == L"lessons"   ? Page::Lessons
             : page == L"lesson"    ? Page::Lesson
             : page == L"quiz"      ? Page::Quiz
             : page == L"questdone" ? Page::QuestDone
             : page == L"practice"  ? Page::Practice
                                    : Page::Home;
    if (m.replay_open) m.page = Page::Look;
    if (o.quest_step >= 0 || m.page == Page::QuestDone) {
        Progress p = m.progress;
        p.quests_done = o.boss ? kQuestsPerBoss - 1 : 0;
        m.quest = make_quest(p);
        m.quest.at = std::clamp(o.quest_step, 0, static_cast<int>(m.quest.steps.size()) - 1);
        for (int i = 0; i < m.quest.at; ++i) {
            m.quest.steps[static_cast<std::size_t>(i)].done = true;
            m.quest.steps[static_cast<std::size_t>(i)].right = i % 3 != 1;
        }
        if (m.quest.boss) {
            m.quest.boss_hp = std::max(1, m.quest.boss_max - 1);
            m.quest.combo = 2;
            m.quest.timer = true;
            m.quest.time_left = 17.4f;
        }
        m.quest.xp = 38;
        if (m.page == Page::QuestDone) {
            m.quest.finished = true;
            for (QuestStep& s : m.quest.steps) s.done = true;
            m.quest.chest_open = o.chest;
        }
    }
    if (o.lesson >= 0 && o.lesson < static_cast<int>(lessons().size())) {
        const Lesson& ls = lessons()[static_cast<std::size_t>(o.lesson)];
        m.lesson = o.lesson;
        m.lesson_code = ls.starter;
        m.lesson_reads_input = lesson_reads_input(ls);
        m.lesson_input = !ls.input.empty() ? ls.input : m.lesson_reads_input ? "7\n" : "";
        m.hint_shown = o.hint;
        if (o.run_lesson) {
            m.lesson_check = check_lesson(ls, m.lesson_code, m.lesson_input);
            m.lesson_ran = true;
        }
    }
    if (o.quiz >= 0) {
        m.quiz = std::min(o.quiz, static_cast<int>(quiz_cards().size()) - 1);
        m.quiz_choice = guess;
    }

    const UINT w = 1180, h = 760;
    const Layout layout = compute_layout(m, static_cast<float>(w), static_cast<float>(h));
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
        return 1;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(renderer.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return 1;
    if (FAILED(renderer.paint(rt.Get(), m, layout, make_palette(dark, system_accent(dark)), false))) return 1;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(out_png.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return 1;
    return 0;
}

}  // namespace

int screenshot(const ShotOptions& o) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = render_png(o);
    CoUninitialize();
    return result;
}

int self_test(const std::wstring& log_path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    const std::filesystem::path progress = std::filesystem::temp_directory_path(ec) / L"CppSparring" / L"selftest-progress.txt";
    std::filesystem::create_directories(progress.parent_path(), ec);
    std::filesystem::remove(progress, ec);
    use_progress_file(progress.wstring());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    auto app = std::make_unique<App>();
    app->renderer.init();
    // never shown: the test runs the real window's code without putting anything on the screen
    HWND hwnd = CreateWindowExW(0, kClassName, L"CppSparring self-test", WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, 0, 0, 1200, 800,
                                nullptr, nullptr, wc.hInstance, app.get());
    if (!hwnd) return 99;
    App& a = *app;
    Model& m = a.model;
    m.progress = load_progress();
    a.relayout();

    std::ofstream log{std::filesystem::path(log_path)};
    int passes = 0, fails = 0;
    auto check = [&](const char* what, bool ok) {
        log << (ok ? "PASS  " : "FAIL  ") << what << "\n";
        log.flush();
        ++(ok ? passes : fails);
    };
    auto pump = [&](const auto& done, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        MSG msg;
        while (std::chrono::steady_clock::now() < end) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (done()) return true;
            Sleep(10);
        }
        return static_cast<bool>(done());
    };
    auto click = [&](Control c, int i = -1) {
        a.click(c, i);
        pump([] { return false; }, 20);
    };
    auto guess_ready = [&] { return !m.importing && m.page == Page::Break && a.step() && a.step()->challenge >= 0; };
    auto ran = [&] {
        return !m.running && m.current >= 0 && m.current < static_cast<int>(m.challenges.size()) &&
               m.challenges[static_cast<std::size_t>(m.current)].ran;
    };
    auto shown = [](HWND h) { return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0; };

    // ---- a whole Daily Quest
    click(Control::HeroQuest);
    check("the big button starts a quest of 5 steps", m.quest.active && m.quest.steps.size() == 5 && !m.quest.boss);
    check("quest step 1 is a quiz card", m.page == Page::Quiz && m.quiz >= 0);
    click(Control::QuizAnswer, 0);
    check("answering the quiz counts the step", m.quiz_choice == 0 && m.quest.steps[0].done);
    click(Control::QuizNext);
    check("step 2 reads a program and picks a challenge", pump(guess_ready, 90000));
    click(Control::Guess, 2);
    check("a guess builds and runs both versions", pump(ran, 90000) && m.original.compiled);
    check("the guess counts in the quest", m.quest.steps[1].done);
    click(Control::NextAfterResult);
    check("step 3 opens a lesson", m.page == Page::Lesson && m.lesson >= 0);
    const Lesson& ls = lessons()[static_cast<std::size_t>(std::max(0, m.lesson))];
    check("the first lesson is C++ lesson 1", ls.id == "cpp01_hello");
    check("the code editor is on the page", shown(a.editor));
    a.set_editor_text("int main() {");
    SendMessageW(a.editor, EM_SETSEL, 12, 12);
    SendMessageW(a.editor, WM_KEYDOWN, VK_RETURN, 0);
    check("Enter after { indents the next line", a.editor_text() == "int main() {\n    ");
    SendMessageW(a.editor, WM_KEYDOWN, VK_TAB, 0);
    check("Tab types four spaces", a.editor_text() == "int main() {\n        ");
    a.set_editor_text(ls.starter);
    click(Control::LessonRun);
    check("the starter code as it is does not pass", pump([&] { return !m.lesson_running && m.lesson_ran; }, 90000) && !m.lesson_check.passed);
    a.set_editor_text("#include <iostream>\n\nint main() {\n    std::cout << \"Hello, C++!\\n\";\n    return 0;\n}\n");
    click(Control::LessonRun);
    check("the right code passes", pump([&] { return !m.lesson_running && m.lesson_ran; }, 90000) && m.lesson_check.passed);
    check("the lesson step counts as right", m.quest.steps[2].done && m.quest.steps[2].right);
    click(Control::LessonNext);
    check("the editor hides when the lesson is left", !shown(a.editor));
    check("step 4 reads a program", pump(guess_ready, 90000));
    click(Control::Guess, 3);
    check("step 4's guess runs", pump(ran, 90000));
    click(Control::NextAfterResult);
    check("step 5 is a quiz card", m.page == Page::Quiz);
    click(Control::QuizAnswer, 1);
    click(Control::QuizNext);
    check("the quest finishes", m.page == Page::QuestDone && m.quest.finished);
    click(Control::ChestOpen);
    check("the chest opens", m.quest.chest_open);
    check("XP was earned", m.progress.xp > 0);
    click(Control::QuestHome);
    check("Take a break goes home and ends the quest", m.page == Page::Home && !m.quest.active);
    check("progress is saved (to the test file)", load_progress().quests_done == 1);

    // ---- the Bug Boss and its timer
    m.progress.quests_done = kQuestsPerBoss - 1;
    click(Control::HeroQuest);
    check("every third quest is a Bug Boss", m.quest.boss && m.quest.boss_hp == kBossHp);
    check("the boss's first guess is ready", pump(guess_ready, 90000));
    click(Control::BossTimer);
    check("the timer switches on", m.quest.timer);
    m.quest.time_left = 0.05f;
    check("when time runs out the step is skipped, no penalty", pump([&] { return m.quest.at >= 1; }, 5000) && m.quest.steps[0].skipped);
    click(Control::QuestQuit);
    check("quitting goes home", m.page == Page::Home && !m.quest.active);

    // ---- Practice with typed answers
    click(Control::NavPractice);
    click(Control::LevelTab, 2);
    int guess_tile = -1;
    for (std::size_t i = 0; i < a.layout.example_items.size(); ++i)
        if (std::string(examples()[a.layout.example_items[i]].file) == "guess_number.cpp") guess_tile = static_cast<int>(i);
    check("Level 3 lists Guess my number", guess_tile >= 0);
    click(Control::Example, guess_tile);
    check("the program is read", pump([&] { return !m.importing && !m.challenges.empty(); }, 90000));
    check("it is seen to read typed answers, which start as 3, 8, 5", m.program_reads_input && m.program_input == "3\n8\n5\n");
    check("the answers box is on the Look page", shown(a.input_edit));
    auto type = [&](const wchar_t* keys) {
        SendMessageW(a.input_edit, EM_SETSEL, 0, -1);
        for (const wchar_t* k = keys; *k; ++k) SendMessageW(a.input_edit, WM_CHAR, *k, 0);
    };
    type(L"9\r5");
    check("typing in the answers box changes the answers", m.program_input == "9\n5");
    SetWindowTextW(a.input_edit, L"3\r\n8\r\n5");   // replaced all at once (as a paste can): read before the next click
    click(Control::StepBreak);
    check("answers set all at once are still used", m.program_input == "3\n8\n5");
    m.current = 0;
    click(Control::Guess, 3);
    check("both versions run", pump(ran, 90000));
    check("the typed answers reach the program", m.original.output.find("You got it in 3 tries") != std::string::npos);

    // ---- watching it run
    click(Control::StepLook);
    click(Control::Watch);
    check("the replay records steps", pump([&] { return !m.replay_loading; }, 90000) && m.replay.ok && m.replay.steps.size() > 5);
    a.set_playing(false);
    a.go_to_step(2);
    check("stepping moves the lit line", m.replay_step == 2 && m.highlight_line == m.replay.steps[2].line);
    bool saw_guess = false;
    for (const ReplayStep& s : m.replay.steps)
        for (const ReplayVar& v : s.vars) saw_guess = saw_guess || v.name == "guess";
    check("the replay shows the boxes' values", saw_guess);
    click(Control::ReplayStop);
    check("Stop watching returns to Look", m.page == Page::Look && !m.replay_open);

    // ---- the other places
    click(Control::NavLearn);
    check("Lessons lists every lesson", m.page == Page::Lessons && a.layout.lesson_tiles.size() == lessons().size() && lessons().size() >= 51);
    click(Control::LessonTile, 0);
    check("a lesson tile opens that lesson", m.page == Page::Lesson);
    click(Control::SessionBack);
    check("Back returns to Lessons", m.page == Page::Lessons);
    click(Control::NavBadges);
    check("Badges shows the quest badge earned", m.page == Page::Badges && m.progress.badges.count("quest1") > 0);
    click(Control::NavHome);
    check("Home is one click away", m.page == Page::Home);

    log << "\n" << passes << " passed, " << fails << " failed\n";
    DestroyWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    std::filesystem::remove(progress, ec);
    CoUninitialize();
    return fails;
}

int run_window(const std::wstring& file) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    auto app = std::make_unique<App>();
    if (!app->renderer.init()) {
        MessageBoxW(nullptr, L"Direct2D or DirectWrite could not be started.", L"CppSparring", MB_ICONERROR);
        return 1;
    }

    const UINT sdpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, kClassName, L"CppSparring", WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1200, static_cast<int>(sdpi), 96), MulDiv(800, static_cast<int>(sdpi), 96), nullptr, nullptr,
                                wc.hInstance, app.get());
    if (!hwnd) return 1;
    app->model.progress = load_progress();
    app->model.pick_level = highest_open_level(app->model.progress);
    app->relayout();
    if (!file.empty()) app->load_file(narrow(file));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

}  // namespace sparring
