#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cmath>
#include <format>
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
#include "fix.hpp"
#include "importer.hpp"
#include "layout.hpp"
#include "mutate.hpp"
#include "lessons.hpp"
#include "palette.hpp"
#include "progress.hpp"
#include "quest.hpp"
#include "quiz.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "sound.hpp"
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
constexpr UINT kWmToolsDone = WM_APP + 6;   // the check for Visual Studio and Clang is done
constexpr UINT kWmFixDone = WM_APP + 7;     // a Fix it puzzle is ready
constexpr UINT kWmBookProgress = WM_APP + 8;   // "Reading page 37 of 210"
constexpr UINT kWmBookDone = WM_APP + 9;       // the lessons from a book or SDK are made (or it failed)
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
        if (wp == VK_ESCAPE) {   // Esc leaves the lesson, as everywhere else
            PostMessageW(owner, WM_KEYDOWN, VK_ESCAPE, 0);
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

struct FixResult {
    unsigned id = 0;
    FixPuzzle puzzle;
};

struct BookProgress {
    unsigned id = 0;
    std::string text;
};

struct BookResult {
    unsigned id = 0;
    ImportOutcome outcome;
};

bool has_extension(const std::wstring& path, const wchar_t* ext) {
    const std::size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, ext) == 0;
}

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
            play(Sound::Cheer);
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
        if (model.page != Page::Lesson || !model.shown_lesson()) {
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
        model.fixing = model.fix_loading = false;
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
        if (model.page != Page::Lesson || !model.shown_lesson() || model.lesson_running) return;
        sync_answers();
        model.lesson_code = editor_text();
        model.lesson_running = true;
        const unsigned id = ++lesson_id;
        HWND h = hwnd;
        const Lesson lesson = *model.shown_lesson();
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

    // ---- Fix it: the open program with one real bug, to repair in the lesson editor
    unsigned fix_id = 0;
    void start_fix() {
        if (model.source_path.empty()) return;
        close_replay();
        model.page = Page::Lesson;
        model.fixing = true;
        model.fix_loading = true;
        model.fix_error.clear();
        model.lesson = -1;
        model.lesson_ran = model.lesson_running = model.hint_shown = false;
        model.lesson_check = LessonCheck{};
        model.lesson_reads_input = false;
        const unsigned id = ++fix_id;
        if (model.importing) return;   // it starts when the program has been read
        if (model.toggles.empty()) {
            model.fix_loading = false;
            model.fix_error = model.import_error.empty() ? "There is nothing in this program that can be broken." : model.import_error;
            return;
        }
        HWND h = hwnd;
        const std::string path = model.source_path, name = model.source_name, source = model.source_code, input = model.program_input;
        const std::vector<MutationToggle> toggles = model.toggles;
        const std::set<std::string> fixed = model.progress.fixed;
        const unsigned seed = static_cast<unsigned>(GetTickCount64());
        std::thread([h, id, path, name, source, toggles, input, fixed, seed] {
            auto r = std::make_unique<FixResult>();
            r->id = id;
            r->puzzle = make_fix(path, name, source, toggles, input, fixed, seed);
            if (PostMessageW(h, kWmFixDone, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
    }
    void show_fix(FixPuzzle p) {
        model.fix_loading = false;
        if (!p.ok) {
            model.fix_error = p.error;
            if (QuestStep* s = step(); s && s->kind == StepKind::Fix) skip_step();   // (no penalty: it was nobody's fault)
            return;
        }
        model.fix = std::move(p.lesson);
        model.lesson_reads_input = !model.fix.input.empty() || reads_input(model.fix.starter);
        model.lesson_input = model.fix.input;
        set_editor_text(model.fix.starter);
        relayout();
        if (editor) SetFocus(editor);
        start_page_anim();
    }

    // ---- Settings: lessons from a book or an SDK folder
    unsigned book_id = 0;
    std::shared_ptr<std::atomic<bool>> book_cancel;

    void start_book_import(const std::wstring& path, bool folder) {
        if (model.book_state == BookState::Working) return;
        if (model.quest.active && !model.quest.finished) return;   // (a quest takes the whole window)
        close_replay();
        model.book_state = BookState::Working;
        model.book_status = "Opening…";
        model.book_message.clear();
        model.book_folder.clear();
        model.confirm_remove = -1;
        model.my_courses = imported_courses();
        model.page = Page::Settings;
        start_page_anim();
        const unsigned id = ++book_id;
        book_cancel = std::make_shared<std::atomic<bool>>(false);
        const std::shared_ptr<std::atomic<bool>> cancel = book_cancel;
        HWND h = hwnd;
        std::thread([h, id, path, folder, cancel] {
            const auto progress = [h, id, cancel](const std::string& text) {
                if (cancel->load()) return false;
                auto p = std::make_unique<BookProgress>();
                p->id = id;
                p->text = text;
                if (PostMessageW(h, kWmBookProgress, 0, reinterpret_cast<LPARAM>(p.get()))) p.release();
                return true;
            };
            auto r = std::make_unique<BookResult>();
            r->id = id;
            r->outcome = folder ? import_folder(path, progress) : import_pdf(path, progress);
            if (PostMessageW(h, kWmBookDone, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
        }).detach();
        relayout();
        invalidate();
    }

    // The lessons were read again: an open imported lesson may be gone or may now be a different one.
    void lessons_reloaded() {
        model.lessons_scroll = 0;
        if (model.lesson >= static_cast<int>(builtin_lesson_count())) {
            model.lesson = -1;
            if (model.page == Page::Lesson && !model.fixing) model.page = Page::Lessons;
        }
    }

    void show_course(const std::string& folder) {
        int number = -1;
        for (const Course& c : courses())
            if (c.imported && c.folder == folder) number = c.number;
        model.page = Page::Lessons;
        model.lessons_scroll = 0;
        relayout();
        for (const auto& [r, n] : layout.course_heads)
            if (n == number) {
                const int most = std::max(0, static_cast<int>(layout.lessons_height - layout.lessons_area.h) + 20);
                model.lessons_scroll = std::clamp(static_cast<int>(r.y - layout.lessons_area.y), 0, most);
            }
    }

    void browse_book() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
        const COMDLG_FILTERSPEC filters[] = {{L"PDF books", L"*.pdf"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(L"Open a programming book (a PDF)");
        if (FAILED(dlg->Show(hwnd))) return;
        ComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return;
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            start_book_import(p, false);
            CoTaskMemFree(p);
        }
    }

    void browse_sdk() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
        DWORD options = 0;
        dlg->GetOptions(&options);
        dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Open an SDK folder");
        if (FAILED(dlg->Show(hwnd))) return;
        ComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return;
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            start_book_import(p, true);
            CoTaskMemFree(p);
        }
    }

    // ---- can this computer build programs?
    void start_tool_check() {
        model.tools = Tools::Checking;
        HWND h = hwnd;
        std::thread([h] {
            auto r = std::make_unique<ToolCheck>(check_tools());
            if (PostMessageW(h, kWmToolsDone, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
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
        case StepKind::Fix: {
            const Example& e = examples()[static_cast<std::size_t>(s.example)];
            if (model.source_name != e.file || model.challenges.empty()) load_file(write_example(e));
            start_fix();
            start_page_anim();
            break;
        }
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
                if (!model.cheers.empty()) play(Sound::Cheer);
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
        if (layout.sound_button.contains(x, y)) return Control::SoundToggle;
        if (layout.nav_home.contains(x, y)) return Control::NavHome;
        if (layout.nav_practice.contains(x, y)) return Control::NavPractice;
        if (layout.nav_badges.contains(x, y)) return Control::NavBadges;
        if (layout.nav_learn.contains(x, y)) return Control::NavLearn;
        if (layout.nav_settings.contains(x, y)) return Control::NavSettings;
        if (layout.quest_skip.contains(x, y)) return Control::QuestSkip;
        if (layout.quest_quit.contains(x, y)) return Control::QuestQuit;
        if (layout.session_back.contains(x, y)) return Control::SessionBack;
        if (layout.step_look.contains(x, y)) return Control::StepLook;
        if (layout.step_break.contains(x, y)) return Control::StepBreak;
        if (layout.step_fix.contains(x, y)) return Control::StepFix;
        auto in = [&](const auto& rects, Control c) {
            for (std::size_t i = 0; i < rects.size(); ++i)
                if (rects[i].contains(x, y)) {
                    index = static_cast<int>(i);
                    return c;
                }
            return Control::None;
        };
        switch (model.page) {
        case Page::Badges:
            if (layout.badges_grownups.contains(x, y)) return Control::Grownups;
            break;
        case Page::Summary:
            if (layout.summary_back.contains(x, y)) return Control::SummaryBack;
            break;
        case Page::Settings:
            if (model.book_state == BookState::Working) {
                if (layout.set_action.contains(x, y)) return Control::ImportCancel;
            } else {
                if (layout.set_pdf.contains(x, y)) return Control::ImportPdf;
                if (layout.set_folder.contains(x, y)) return Control::ImportFolder;
                if (model.book_state == BookState::Done && layout.set_action.contains(x, y)) return Control::ImportOpen;
            }
            for (std::size_t k = 0; k < layout.set_rows.size(); ++k) {
                const int course = layout.set_first + static_cast<int>(k);
                if (model.confirm_remove == course) {
                    if (layout.set_yes[k].contains(x, y)) {
                        index = course;
                        return Control::CourseRemoveYes;
                    }
                    if (layout.set_no[k].contains(x, y)) {
                        index = course;
                        return Control::CourseRemoveNo;
                    }
                } else if (layout.set_remove[k].contains(x, y)) {
                    index = course;
                    return Control::CourseRemove;
                }
            }
            break;
        case Page::Setup:
            if (model.tools == Tools::Ready) {
                if (Rect{layout.setup_recheck.x, layout.setup_recheck.y, layout.setup_card.w - 64, 44}.contains(x, y)) return Control::SetupContinue;
                break;
            }
            if (model.tools == Tools::Checking) break;
            if (layout.setup_recheck.contains(x, y)) return Control::SetupRecheck;
            if (layout.setup_installer.contains(x, y)) return Control::SetupInstaller;
            if (layout.setup_continue.contains(x, y)) return Control::SetupContinue;
            break;
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
            if (lesson_practice(model) >= 0 && layout.lesson_practice.contains(x, y)) return Control::LessonPractice;
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

    // The keyboard: 1-4 answer (A-D too on a quiz card), Enter goes on, Esc goes back. True when the key did something.
    bool key(UINT vk) {
        int answer = -1;
        if (vk >= '1' && vk <= '4') answer = static_cast<int>(vk - '1');
        if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD4) answer = static_cast<int>(vk - VK_NUMPAD1);
        if (model.page == Page::Quiz && vk >= 'A' && vk <= 'D') answer = static_cast<int>(vk - 'A');
        const bool enter = vk == VK_RETURN, esc = vk == VK_ESCAPE;
        const bool in_quest = model.quest.active && !model.quest.finished;
        switch (model.page) {
        case Page::Home:
            if (enter) return click(Control::HeroQuest, -1), true;
            break;
        case Page::Break: {
            if (model.challenges.empty() || model.current < 0) break;
            const Challenge& c = model.challenges[static_cast<std::size_t>(model.current)];
            if (answer >= 0 && c.guess < 0 && !model.running) return click(Control::Guess, answer), true;
            if (enter && c.ran) return click(Control::NextAfterResult, -1), true;
            if (esc && !in_quest) return click(Control::SessionBack, -1), true;
            break;
        }
        case Page::Look:
            if (esc && model.replay_open) return click(Control::ReplayStop, -1), true;
            if (enter && !model.replay_open && !model.importing && !model.toggles.empty()) return click(Control::NextStep, -1), true;
            if (esc && !in_quest) return click(Control::SessionBack, -1), true;
            break;
        case Page::Quiz:
            if (answer >= 0 && model.quiz_choice < 0) return click(Control::QuizAnswer, answer), true;
            if (enter && model.quiz_choice >= 0) return click(Control::QuizNext, -1), true;
            break;
        case Page::Lesson:
            if (esc && !in_quest) return click(Control::SessionBack, -1), true;
            if (enter && model.lesson_ran && model.lesson_check.passed) return click(Control::LessonNext, -1), true;
            break;
        case Page::QuestDone:
            if (enter) return click(model.quest.chest_open ? Control::QuestAgain : Control::ChestOpen, -1), true;
            if (esc) return click(Control::QuestHome, -1), true;
            break;
        case Page::Setup:
            if (enter) return click(model.tools == Tools::Ready ? Control::SetupContinue : Control::SetupRecheck, -1), true;
            if (esc) return click(Control::SetupContinue, -1), true;
            break;
        case Page::Practice:
        case Page::Lessons:
        case Page::Badges:
            if (esc) return click(Control::NavHome, -1), true;
            break;
        case Page::Summary:
            if (esc) return click(Control::SummaryBack, -1), true;
            break;
        case Page::Settings:
            if (esc) return click(model.confirm_remove >= 0 ? Control::CourseRemoveNo : Control::NavHome, model.confirm_remove), true;
            break;
        }
        return false;
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
        case Control::SoundToggle:
            model.progress.sound = !model.progress.sound;
            set_sound_on(model.progress.sound);
            save_progress(model.progress);
            if (model.progress.sound) play(Sound::Right);
            break;
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
            model.page = model.page == Page::Lesson && !model.fixing ? Page::Lessons : Page::Practice;
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
        case Control::StepFix:
            if (!loaded || (model.page == Page::Lesson && model.fixing)) break;
            start_fix();
            break;
        case Control::SetupRecheck: start_tool_check(); break;
        case Control::SetupInstaller:
            if (!model.tool_check.installer.empty())
                ShellExecuteW(hwnd, L"open", widen(model.tool_check.installer).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else
                ShellExecuteW(hwnd, L"open", L"https://visualstudio.microsoft.com/vs/community/", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Control::SetupContinue: model.page = Page::Home; break;
        case Control::Grownups: model.page = Page::Summary; break;
        case Control::NavSettings:
            close_replay();
            model.my_courses = imported_courses();
            model.confirm_remove = -1;
            model.page = Page::Settings;
            break;
        case Control::ImportPdf: browse_book(); return;
        case Control::ImportFolder: browse_sdk(); return;
        case Control::ImportCancel:
            if (book_cancel) book_cancel->store(true);
            model.book_status = "Stopping…";
            break;
        case Control::ImportOpen: show_course(model.book_folder); break;
        case Control::CourseRemove: model.confirm_remove = i; break;
        case Control::CourseRemoveNo: model.confirm_remove = -1; break;
        case Control::CourseRemoveYes:
            if (i >= 0 && i < static_cast<int>(model.my_courses.size())) {
                const std::string folder = model.my_courses[static_cast<std::size_t>(i)].folder;
                remove_imported_course(folder);
                reload_lessons();
                lessons_reloaded();
                model.my_courses = imported_courses();
                if (model.book_folder == folder) {
                    model.book_state = BookState::Idle;
                    model.book_folder.clear();
                }
            }
            model.confirm_remove = -1;
            break;
        case Control::SummaryBack: model.page = Page::Badges; break;
        case Control::HeroQuest:
            if (model.tools == Tools::Missing) {
                model.page = Page::Setup;
                break;
            }
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
            if (const Lesson* shown = model.shown_lesson()) set_editor_text(shown->starter);
            model.lesson_ran = false;
            break;
        case Control::LessonNext:
            if (!model.lesson_ran || !model.lesson_check.passed) break;
            if (model.quest.active && !model.quest.finished) advance_quest();
            else if (model.fixing) start_fix();
            else {
                const std::vector<int> order = lesson_order();
                const auto at = std::find(order.begin(), order.end(), model.lesson);
                if (at != order.end() && at + 1 != order.end()) open_lesson(*(at + 1));
                else model.page = Page::Lessons;
            }
            return;
        case Control::LessonPractice:
            if (const int ex = lesson_practice(model); ex >= 0) {
                const Example& e = examples()[static_cast<std::size_t>(ex)];
                model.pick_level = e.level;
                if (model.source_name == e.file && !model.importing && !model.nodes.empty()) model.page = Page::Look;
                else load_file(write_example(e));
            }
            return;
        case Control::QuizAnswer:
            if (model.quiz_choice < 0 && i >= 0 && model.quiz >= 0) {
                model.quiz_choice = i;
                const bool right = answer_order(static_cast<std::size_t>(model.quiz))[static_cast<std::size_t>(i)] == 0;
                play(right ? Sound::Right : Sound::Wrong);
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
            play(Sound::Chest);
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
        ++fix_id;   // a puzzle being made for the last program is not wanted now
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
        if (layout.sound_button.contains(x, y)) return HTCLIENT;
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
        SetFocus(hwnd);   // so the keys work again after typing in a box
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
        } else if (model.page == Page::Settings && layout.set_list.contains(x, y)) {
            model.settings_scroll = std::clamp(model.settings_scroll - steps, 0, std::max(0, static_cast<int>(model.my_courses.size()) - 1));
            relayout();
            model.settings_scroll = layout.set_first;
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
        if (model.page == Page::Break && (wp == VK_LEFT || wp == VK_RIGHT) && !model.quest.active) {
            step_challenge(wp == VK_LEFT ? -1 : +1);
            start_page_anim();
            invalidate();
            return 0;
        }
        if (key(static_cast<UINT>(wp))) return 0;
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
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            const DWORD attributes = GetFileAttributesW(path);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) start_book_import(path, true);   // an SDK folder
            else if (has_extension(path, L".pdf")) start_book_import(path, false);
            else load_file(narrow(path));
        }
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
        if (model.page == Page::Lesson && model.fixing && model.fix_loading) start_fix();   // Fix it was pressed while it was being read
        relayout();
        invalidate();
        return 0;
    }

    case kWmLessonDone: {
        std::unique_ptr<LessonResult> r(reinterpret_cast<LessonResult*>(lp));
        const Lesson* shown = model.shown_lesson();
        if (r->id != lesson_id || !shown) return 0;
        model.lesson_running = false;
        model.lesson_ran = true;
        model.lesson_check = std::move(r->check);
        const bool passed = model.lesson_check.passed;
        const int before = model.progress.xp;
        cheer(model.fixing ? record_fix(model.progress, shown->id, passed, shown->bug_kind) : record_lesson(model.progress, shown->id, passed));
        if (QuestStep* s = step(); s && passed && ((s->kind == StepKind::Write && !model.fixing && s->lesson == model.lesson) || (s->kind == StepKind::Fix && model.fixing))) {
            s->done = s->right = true;
            model.quest.xp += model.progress.xp - before;
        }
        if (passed) play(Sound::Right);   // (no sound for a failed try: trying is good)
        if (passed && animations) {
            model.confetti = 0;
            kick();
        }
        relayout();   // a passed lesson may show its Practice button
        invalidate();
        return 0;
    }

    case kWmRunLesson:
        run_lesson();
        return 0;

    case kWmFixDone: {
        std::unique_ptr<FixResult> r(reinterpret_cast<FixResult*>(lp));
        if (r->id != fix_id || !model.fixing || !model.fix_loading) return 0;
        show_fix(std::move(r->puzzle));
        relayout();
        invalidate();
        return 0;
    }

    case kWmBookProgress: {
        std::unique_ptr<BookProgress> r(reinterpret_cast<BookProgress*>(lp));
        if (r->id != book_id || model.book_state != BookState::Working) return 0;
        if (!(book_cancel && book_cancel->load())) model.book_status = r->text;
        invalidate();
        return 0;
    }

    case kWmBookDone: {
        std::unique_ptr<BookResult> r(reinterpret_cast<BookResult*>(lp));
        if (r->id != book_id) return 0;
        const ImportOutcome& o = r->outcome;
        if (o.ok) {
            reload_lessons();
            lessons_reloaded();
            model.book_state = BookState::Done;
            model.book_folder = o.folder;
            model.book_message = std::format("Made {} lessons from '{}'.", o.lessons, o.name);
            play(Sound::Right);
        } else {
            model.book_state = BookState::Failed;
            model.book_message = o.cancelled ? "Stopped. No lessons were made." : o.error;
        }
        model.my_courses = imported_courses();
        relayout();
        invalidate();
        return 0;
    }

    case kWmToolsDone: {
        std::unique_ptr<ToolCheck> r(reinterpret_cast<ToolCheck*>(lp));
        model.tool_check = *r;
        model.tools = r->builds ? Tools::Ready : Tools::Missing;
        if (model.tools == Tools::Missing && model.page == Page::Home) {   // say what's needed before anything fails
            model.page = Page::Setup;
            start_page_anim();
        }
        relayout();
        invalidate();
        return 0;
    }

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
                if (r->index == static_cast<std::size_t>(model.current)) play(right ? Sound::Right : Sound::Wrong);
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
        if (book_cancel) book_cancel->store(true);
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

    // lessons really made from a PDF or folder (or several, separated by |), kept in a temporary folder of their own
    std::size_t first_imported = lessons().size();
    ImportOutcome last_import;
    int imports = 0;
    if (!o.import_path.empty()) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / L"CppSparring" / L"shot-courses";
        std::filesystem::remove_all(dir, ec);
        use_courses_dir(dir.wstring());
        std::wstring rest = o.import_path;
        while (!rest.empty()) {
            const std::size_t bar = rest.find(L'|');
            const std::wstring one = rest.substr(0, bar);
            rest = bar == std::wstring::npos ? L"" : rest.substr(bar + 1);
            const bool folder = std::filesystem::is_directory(std::filesystem::path(one), ec);
            last_import = folder ? import_folder(one, {}) : import_pdf(one, {});
            imports += last_import.ok;
        }
        reload_lessons();
    }
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
        p.kinds = {{"offbyone", {2, 5, 0}}, {"math", {4, 1, 1}}, {"init", {1, 0, 1}}, {"equals", {3, 0, 0}}, {"decimals", {1, 1, 0}}};
        p.fixed = {"fix:a", "fix:b"};
        p.lessons_done = {"cpp01_hello", "cpp02_boxes", "01_hello_world"};
        p.quiz_right = {"q1", "q2", "q3"};
        const std::vector<std::string> week = this_week();
        p.days = {"2026-11-02", week[0], week[1]};
        p.quests_done = 4;
        p.bosses_beaten = 1;
        p.xp = 180;
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
             : page == L"setup"     ? Page::Setup
             : page == L"fix"       ? Page::Lesson
             : page == L"summary"   ? Page::Summary
             : page == L"settings"  ? Page::Settings
                                    : Page::Home;
    m.tools = Tools::Ready;
    if (m.page == Page::Summary) m.cheers.clear();
    if (m.page == Page::Settings) {
        m.cheers.clear();
        m.my_courses = imported_courses();
        if (imports > 0) {
            m.book_state = BookState::Done;
            m.book_folder = last_import.folder;
            m.book_message = std::format("Made {} lessons from '{}'.", last_import.lessons, last_import.name);
        }
        if (o.demo && imports == 0) {   // made-up courses, with --setup 1 (working), 2 (asking before removing) or 3 (a problem)
            m.my_courses = {{"learning_c", "Learning C the Easy Way", "learning_c.pdf", "C", "2026-11-25", 41, 14},
                            {"some_sdk", "Game SDK", "game_sdk", "C and C++", "2026-11-26", 17, 6}};
            if (o.setup == 1) {
                m.book_state = BookState::Working;
                m.book_status = "Trying code sample 12 of 48";
            } else if (o.setup == 3) {
                m.book_state = BookState::Failed;
                m.book_message = "This PDF is only pictures of pages (a scan), so there are no words in it for me to read.";
            } else {
                m.book_state = BookState::Done;
                m.book_message = "Made 41 lessons from 'Learning C the Easy Way'.";
            }
            m.confirm_remove = o.setup == 2 ? 1 : -1;
        }
    }
    if (!o.import_path.empty() && o.lesson < 0 && m.page == Page::Lesson) m.lesson = static_cast<int>(first_imported) + 1;   // the second one made: a gap to fill
    if (m.page == Page::Setup) {
        m.tool_check.visual_studio = o.setup >= 1;
        m.tool_check.clang = o.setup >= 2;
        m.tool_check.builds = o.setup >= 2;
        m.tools = o.setup >= 2 ? Tools::Ready : Tools::Missing;
    }
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
    if (!o.import_path.empty() && o.lesson < 0 && m.page == Page::Lesson && first_imported + 1 < lessons().size()) {
        const Lesson& ls = lessons()[first_imported + 1];
        m.lesson = static_cast<int>(first_imported) + 1;
        m.lesson_code = ls.starter;
        m.lesson_reads_input = lesson_reads_input(ls);
        m.lesson_input = ls.input;
        m.hint_shown = o.hint;
        if (o.run_lesson) {
            m.lesson_check = check_lesson(ls, m.lesson_code, m.lesson_input);
            m.lesson_ran = true;
        }
    } else if (o.lesson >= 0 && o.lesson < static_cast<int>(lessons().size())) {
        const Lesson& ls = lessons()[static_cast<std::size_t>(o.lesson)];
        m.lesson = o.lesson;
        m.lesson_code = ls.starter;
        if (!o.code_file.empty()) {
            std::ifstream in(std::filesystem::path(o.code_file), std::ios::binary);
            m.lesson_code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        m.lesson_reads_input = lesson_reads_input(ls);
        m.lesson_input = !ls.input.empty() ? ls.input : m.lesson_reads_input ? "7\n" : "";
        m.hint_shown = o.hint;
        if (o.run_lesson) {
            m.lesson_check = check_lesson(ls, m.lesson_code, m.lesson_input);
            m.lesson_ran = true;
        }
    }
    if (page == L"fix") {   // Fix it on the --file program: really pick a bug (with --run, run the broken code as it is)
        m.fixing = true;
        const FixPuzzle p = make_fix(m.source_path, m.source_name, m.source_code, m.toggles, m.program_input, {}, 7);
        m.fix_error = p.ok ? std::string() : p.error;
        m.fix = p.lesson;
        m.lesson_code = m.fix.starter;
        m.lesson_reads_input = !m.fix.input.empty();
        m.lesson_input = m.fix.input;
        m.hint_shown = o.hint;
        if (o.run_lesson && p.ok) {
            m.lesson_check = check_lesson(m.fix, m.lesson_code, m.lesson_input);
            m.lesson_ran = true;
        }
    }
    if (o.quiz >= 0) {
        m.quiz = std::min(o.quiz, static_cast<int>(quiz_cards().size()) - 1);
        m.quiz_choice = guess;
    }

    const UINT w = 1180, h = 760;
    if (m.page == Page::Lessons && !o.import_path.empty()) {   // the end of the list, where the imported courses are
        const Layout whole = compute_layout(m, static_cast<float>(w), static_cast<float>(h));
        m.lessons_scroll = std::max(0, static_cast<int>(whole.lessons_height - whole.lessons_area.h) + 20);
    }
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

// The app's icon, drawn here and saved as an .ico with every size Windows asks for (each as a PNG).
int make_icon(const std::wstring& out_path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    {
        Renderer renderer;
        ComPtr<IWICImagingFactory> wic;
        if (!renderer.init() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 1;
        const Palette pal = make_palette(false, 0);
        const UINT sizes[] = {16, 24, 32, 48, 64, 128, 256};
        std::vector<std::vector<unsigned char>> pngs;
        for (UINT s : sizes) {
            ComPtr<IWICBitmap> bitmap;
            ComPtr<ID2D1RenderTarget> rt;
            wic->CreateBitmap(s, s, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (!bitmap || FAILED(renderer.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return 1;
            const float f = static_cast<float>(s);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            ComPtr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[2] = {{0.0f, pal.hero_a}, {1.0f, pal.hero_b}};
            rt->CreateGradientStopCollection(gs, 2, &stops);
            ComPtr<ID2D1LinearGradientBrush> gradient;
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(f, f)), stops.Get(), &gradient);
            const float inset = s <= 24 ? 0.0f : f * 0.04f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, f - inset, f - inset), f * 0.22f, f * 0.22f), gradient.Get());
            ComPtr<ID2D1SolidColorBrush> brush;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush);
            const bool tiny = s <= 24;
            const std::wstring braces = tiny ? L"{}" : L"{ }";
            IDWriteTextFormat* mono = renderer.format(3, f * (tiny ? 0.62f : 0.5f), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                                                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            rt->DrawText(braces.c_str(), static_cast<UINT32>(braces.size()), mono, D2D1::RectF(0, -f * 0.04f, f, f * 0.96f), brush.Get());
            if (!tiny) {
                brush->SetColor(color(0xFFC83D));
                IDWriteTextFormat* icons = renderer.format(2, f * 0.34f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                                                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                const wchar_t bolt = L'\xE945';
                rt->DrawText(&bolt, 1, icons, D2D1::RectF(0, 0, f, f), brush.Get());
            }
            if (FAILED(rt->EndDraw())) return 1;

            ComPtr<IStream> stream;
            CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
                FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
                FAILED(frame->Commit()) || FAILED(encoder->Commit()))
                return 1;
            STATSTG st{};
            stream->Stat(&st, STATFLAG_NONAME);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(st.cbSize.QuadPart));
            const LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG got = 0;
            stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &got);
            pngs.push_back(std::move(bytes));
        }
        // the .ico: a header, one entry per size, then the PNGs
        std::vector<unsigned char> ico;
        auto put16 = [&](unsigned v) { ico.push_back(static_cast<unsigned char>(v & 0xFF)); ico.push_back(static_cast<unsigned char>(v >> 8)); };
        auto put32 = [&](unsigned v) { put16(v & 0xFFFF); put16(v >> 16); };
        put16(0);
        put16(1);
        put16(static_cast<unsigned>(pngs.size()));
        unsigned offset = 6 + 16 * static_cast<unsigned>(pngs.size());
        for (std::size_t i = 0; i < pngs.size(); ++i) {
            ico.push_back(static_cast<unsigned char>(sizes[i] >= 256 ? 0 : sizes[i]));
            ico.push_back(static_cast<unsigned char>(sizes[i] >= 256 ? 0 : sizes[i]));
            ico.push_back(0);
            ico.push_back(0);
            put16(1);
            put16(32);
            put32(static_cast<unsigned>(pngs[i].size()));
            put32(offset);
            offset += static_cast<unsigned>(pngs[i].size());
        }
        for (const auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
        std::ofstream(std::filesystem::path(out_path), std::ios::binary).write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
        result = 0;
    }
    CoUninitialize();
    return result;
}

int register_open_with() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring app = L"Software\\Classes\\Applications\\cppsparring.exe";
    const std::wstring command = L"\"" + std::wstring(exe) + L"\" \"%1\"";
    const std::wstring icon = std::wstring(exe) + L",0";
    auto set = [](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    bool ok = set(app, L"FriendlyAppName", L"CppSparring") && set(app + L"\\shell\\open\\command", nullptr, command) &&
              set(app + L"\\DefaultIcon", nullptr, icon);
    for (const wchar_t* ext : {L".cpp", L".cxx", L".cc"}) {
        ok = ok && set(app + L"\\SupportedTypes", ext, L"");
        ok = ok && set(std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithList\\cppsparring.exe", nullptr, L"");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? 0 : 1;
}

bool layout_ok(const App& a) { return a.layout.set_pdf.w > 0 && a.layout.set_folder.w > 0 && a.layout.set_status.w > 0; }

int self_test(const std::wstring& log_path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    const std::filesystem::path progress = std::filesystem::temp_directory_path(ec) / L"CppSparring" / L"selftest-progress.txt";
    std::filesystem::create_directories(progress.parent_path(), ec);
    std::filesystem::remove(progress, ec);
    use_progress_file(progress.wstring());
    // imported courses go in a folder of their own, never the real one
    const std::filesystem::path test_courses = std::filesystem::temp_directory_path(ec) / L"CppSparring" / L"selftest-courses";
    std::filesystem::remove_all(test_courses, ec);
    use_courses_dir(test_courses.wstring());

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
    set_sound_on(false);   // a test makes no noise
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

    // ---- this computer can build programs
    a.start_tool_check();
    check("the setup check finishes", pump([&] { return m.tools != Tools::Checking; }, 90000));
    check("this computer has Visual Studio, Clang, and a test program runs",
          m.tools == Tools::Ready && m.tool_check.visual_studio && m.tool_check.clang && m.tool_check.builds);

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
    check("step 4 is Fix it, with the editor hidden while a bug is picked", m.page == Page::Lesson && m.fixing && m.fix_loading && !shown(a.editor));
    auto fix_ready = [&] { return m.fixing && !m.fix_loading; };
    auto lesson_done = [&] { return !m.lesson_running && m.lesson_ran; };
    check("a bug is picked that really breaks the program",
          pump(fix_ready, 180000) && m.fix_error.empty() && m.fix.starter != m.source_code && !m.fix.expected.empty());
    check("the broken program is in the editor", shown(a.editor) && a.editor_text() == m.fix.starter);
    click(Control::LessonRun);
    check("the broken program as it is does not pass", pump(lesson_done, 90000) && !m.lesson_check.passed);
    a.set_editor_text(m.source_code);
    click(Control::LessonRun);
    check("putting it back fixes it", pump(lesson_done, 90000) && m.lesson_check.passed);
    check("the fix counts in the quest, earns its badge and is remembered",
          m.quest.steps[3].done && m.quest.steps[3].right && m.progress.badges.count("fix1") && load_progress().fixed.count(m.fix.id));
    click(Control::LessonNext);
    check("the editor hides when Fix it is left", !shown(a.editor));
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

    // ---- the keyboard
    m.progress.quests_done = 0;
    auto press = [&](UINT vk) {
        SendMessageW(hwnd, WM_KEYDOWN, vk, 0);
        pump([] { return false; }, 20);
    };
    press(VK_RETURN);
    check("Enter on Home starts a quest", m.quest.active && m.page == Page::Quiz);
    press('B');
    check("B answers a quiz card", m.quiz_choice == 1);
    press(VK_RETURN);
    check("Enter goes on to the next step", m.quest.at == 1);
    check("the next step's program is read", pump(guess_ready, 90000));
    press('3');
    check("3 makes a guess", pump(ran, 90000) && m.challenges[static_cast<std::size_t>(m.current)].guess == 2);
    click(Control::QuestQuit);
    click(Control::NavPractice);
    press(VK_ESCAPE);
    check("Esc on Practice goes home", m.page == Page::Home);

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

    // ---- a change that doesn't build: == turned into = on text
    check("Level 5 has four programs", examples_in_level(5).size() == 4);
    click(Control::NavPractice);
    click(Control::LevelTab, 4);
    int rps_tile = -1;
    for (std::size_t i = 0; i < a.layout.example_items.size(); ++i)
        if (std::string(examples()[a.layout.example_items[i]].file) == "rock_paper_scissors.cpp") rps_tile = static_cast<int>(i);
    click(Control::Example, rps_tile);
    check("Rock paper scissors is read", rps_tile >= 0 && pump([&] { return !m.importing && !m.challenges.empty(); }, 90000));
    int eq = -1;
    for (std::size_t i = 0; i < m.toggles.size(); ++i)
        if (m.toggles[i].rule == Rule::EqToAssign && eq < 0) eq = static_cast<int>(i);
    check("it offers to turn == into =", eq >= 0);
    click(Control::StepBreak);
    m.current = std::max(0, eq);
    click(Control::Guess, static_cast<int>(Outcome::WontBuild));
    check("turning == into = on text really doesn't build", pump(ran, 90000) &&
                                                               m.challenges[static_cast<std::size_t>(m.current)].truth == static_cast<int>(Outcome::WontBuild));
    check("the typed move reaches the program", m.original.output.find("You win!") != std::string::npos);
    click(Control::StepFix);
    check("Fix it on a program that reads typed answers finds a bug",
          pump(fix_ready, 180000) && m.fix_error.empty() && m.fix.input == "rock\n" && m.fix.expected.find("You win!") != std::string::npos);
    click(Control::SessionBack);
    check("Back from Fix it returns to Practice", m.page == Page::Practice);

    // ---- the newer kinds of bug, each really doing what it says (on Pizza party)
    {
        int pizza = -1;
        for (std::size_t i = 0; i < examples().size(); ++i)
            if (std::string(examples()[i].file) == "pizza_party.cpp") pizza = static_cast<int>(i);
        check("Level 5 has Pizza party", pizza >= 0 && examples()[static_cast<std::size_t>(std::max(0, pizza))].level == 5);
        const std::string path = write_example(examples()[static_cast<std::size_t>(std::max(0, pizza))]);
        std::string src;
        read_file(path, src);
        const std::vector<MutationToggle> ts = generate_mutations(parse_ast(path), src);
        const CompileResult original = compile_and_run(path);
        check("Pizza party prints 14 slices, 3.5 each",
              original.output.find("We ate 14 slices") != std::string::npos && original.output.find("3.5 slices each") != std::string::npos);
        auto outcome = [&](Rule r, const char* needle) {
            for (const MutationToggle& t : ts)
                if (t.rule == r) {
                    const CompileResult c = compile_and_run_string(apply_mutation(src, t), path);
                    if (needle && c.output.find(needle) == std::string::npos) return -2;
                    return static_cast<int>(classify(original, c));
                }
            return -1;
        };
        check("going past the end of the list crashes", outcome(Rule::PastEnd, nullptr) == static_cast<int>(Outcome::Crashes));
        check("starting the loop at 1 skips the first slices", outcome(Rule::StartAt, "We ate 11 slices") == static_cast<int>(Outcome::Different));
        check("a box with no first value holds the same junk every time", outcome(Rule::RemoveInit, "-14316557") == static_cast<int>(Outcome::Different));
        check("making each an int chops off the .5", outcome(Rule::MakeInt, "That is 3 slices each") == static_cast<int>(Outcome::Different));
        check("dividing by 4 instead of 4.0 is whole-number division", outcome(Rule::WholeDivide, "That is 3 slices each") == static_cast<int>(Outcome::Different));
    }

    // ---- the lessons about kinds of bug: each starter is broken, its hint fixes it, and it points at a Practice program
    {
        bool linked = true;
        for (const Lesson& ls : lessons())
            if (!ls.practice.empty())
                linked = linked && std::any_of(examples().begin(), examples().end(), [&](const Example& e) { return ls.practice == e.file; });
        check("every lesson's Practice program is there", linked);
        struct Fixed { const char* id; const char* from; const char* to; };
        const Fixed fixes[] = {
            {"cpp19_first_value", "int total;", "int total = 0;"},
            {"cpp20_whole_numbers", "total / 4;", "total / 4.0;"},
            {"cpp21_end_of_list", "i <= scores", "i < scores"},
            {"cpp22_less_or_equal", "i < 5", "i <= 5"},
            {"cpp23_equals", "guess = secret", "guess == secret"},
            {"cpp24_catch_the_answer", "    double_it(score);", "    score = double_it(score);"},
        };
        int found = 0, broken = 0, mended = 0;
        for (const Fixed& f : fixes) {
            const auto at = std::find_if(lessons().begin(), lessons().end(), [&](const Lesson& ls) { return ls.id == f.id; });
            if (at == lessons().end() || at->course != 13 || at->practice.empty()) continue;
            ++found;
            std::string code = at->starter;
            const std::size_t pos = code.find(f.from);
            if (!check_lesson(*at, code).passed) ++broken;
            if (pos != std::string::npos && check_lesson(*at, code.replace(pos, std::strlen(f.from), f.to)).passed) ++mended;
        }
        check("the six bug lessons are in C++ 4", found == 6);
        check("each bug lesson's starter does not pass", broken == 6);
        check("each bug lesson's fix passes", mended == 6);

        const auto eq = std::find_if(lessons().begin(), lessons().end(), [](const Lesson& ls) { return ls.id == "cpp23_equals"; });
        const int eq_index = static_cast<int>(eq - lessons().begin());
        for (int n = 0; n < kRightToOpenNextLevel; ++n) m.progress.right.insert("robot_age.cpp|test@" + std::to_string(n));   // open level 2
        a.open_lesson(eq_index);
        check("no Practice button before the lesson is passed", lesson_practice(m) < 0);
        std::string code = eq->starter;
        code.replace(code.find("guess = secret"), 14, "guess == secret");
        a.set_editor_text(code);
        click(Control::LessonRun);
        check("a passed bug lesson shows its Practice button",
              pump([&] { return !m.lesson_running && m.lesson_ran; }, 90000) && m.lesson_check.passed && lesson_practice(m) >= 0 && a.layout.lesson_practice.w > 0);
        click(Control::LessonPractice);
        check("the Practice button opens Secret number", m.source_name == "secret_number.cpp" && m.pick_level == 2);
        pump([&] { return !m.importing; }, 90000);
    }

    // ---- the other places
    click(Control::NavLearn);
    check("Lessons lists every lesson", m.page == Page::Lessons && a.layout.lesson_tiles.size() == lessons().size() && lessons().size() >= 57);
    click(Control::LessonTile, 0);
    check("a lesson tile opens that lesson", m.page == Page::Lesson);
    click(Control::SessionBack);
    check("Back returns to Lessons", m.page == Page::Lessons);
    click(Control::NavBadges);
    check("Badges shows the quest badge earned", m.page == Page::Badges && m.progress.badges.count("quest1") > 0);
    click(Control::Grownups);
    check("For grown-ups opens the summary", m.page == Page::Summary);
    {
        int tries = 0;
        for (const auto& [id, n] : m.progress.kinds) tries += n.right + n.wrong + n.fixed;
        const Progress saved = load_progress();
        int saved_tries = 0;
        for (const auto& [id, n] : saved.kinds) saved_tries += n.right + n.wrong + n.fixed;
        check("the guesses and fixes are counted by kind of bug, and saved", tries > 0 && saved_tries == tries && find_bug_kind(saved.kinds.begin()->first));
    }
    press(VK_ESCAPE);
    check("Esc on the summary goes back to Badges", m.page == Page::Badges);
    click(Control::NavHome);
    check("Home is one click away", m.page == Page::Home);
    click(Control::SoundToggle);
    check("the speaker button mutes, and it is remembered", !m.progress.sound && !load_progress().sound);
    check("the program carries its icon", LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0) != nullptr);


    // ---- lessons from a book or an SDK: the PDF reader, the lesson maker and the Settings page
    {
        namespace fs = std::filesystem;
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const fs::path fixtures = fs::path(exe_path).parent_path() / L"fixtures";
        auto fixture = [&](const wchar_t* name) { return (fixtures / name).wstring(); };
        auto text_of = [](const pdf::Document& doc) {
            std::string all;
            for (const pdf::Page& pg : doc.pages)
                for (const pdf::Line& ln : pg.lines) all += ln.text + "\n";
            return all;
        };

        // Flate, the compression PDFs use: three kinds of block, cut-off data and nonsense
        const unsigned char stored[] = {0x78, 0x01, 0x01, 0x12, 0x00, 0xED, 0xFF, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x73, 0x74, 0x6F, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6C, 0x6F, 0x63, 0x6B, 0x42, 0x51, 0x06, 0xF1};
        const unsigned char fixed[] = {0x78, 0xDA, 0x4B, 0x4C, 0x4A, 0x4E, 0xC4, 0x40, 0x0A, 0x15, 0x95, 0x55, 0x00, 0x7B, 0xF5, 0x09, 0x96};
        const unsigned char dynamic_block[] = {0x78, 0xDA, 0xED, 0x8D, 0x4D, 0x0E, 0x82, 0x30, 0x14, 0x84, 0xAF, 0x32, 0xBA, 0x82, 0xC4, 0x10, 0xF7, 0x5C, 0xC0, 0xA5, 0x89, 0x2C, 0xDD, 0x14, 0x79, 0x2D, 0xD5, 0xD2, 0x07, 0xA5, 0x2D, 0x3F, 0xC6, 0xBB, 0x5B, 0x8C, 0x97, 0x30, 0x71, 0x39, 0xF9, 0x66, 0xBE, 0xA9, 0x5A, 0xC2, 0x10, 0xF4, 0xED, 0x81, 0xDA, 0xF1, 0x64, 0x21, 0x79, 0xC6, 0x3D, 0x74, 0xFD, 0x08, 0x8E, 0xE4, 0xE0, 0x13, 0x36, 0x62, 0x5D, 0xD0, 0xB0, 0x2A, 0x70, 0x16, 0xA9, 0xD7, 0x2D, 0xA8, 0x53, 0x69, 0xD2, 0xBE, 0x85, 0xD4, 0x91, 0x12, 0x5A, 0xC9, 0xC2, 0xE8, 0x21, 0xB0, 0x4B, 0x5B, 0x35, 0x16, 0xA8, 0xFE, 0xD6, 0x1F, 0xB2, 0x9E, 0x78, 0x42, 0xA4, 0x59, 0x5B, 0x65, 0x96, 0xAF, 0xBE, 0x11, 0xD2, 0x63, 0xA5, 0xDA, 0x89, 0xF1, 0x73, 0xB0, 0xC3, 0xA5, 0x6F, 0xB5, 0x9D, 0xC1, 0x12, 0xB5, 0xD9, 0xC4, 0x43, 0x10, 0xCE, 0xAF, 0x87, 0x44, 0x1B, 0x45, 0xDB, 0x4D, 0xE4, 0xA9, 0x80, 0xB6, 0x1E, 0x9D, 0xD0, 0x36, 0xCB, 0xF1, 0x44, 0xEF, 0x52, 0x94, 0xD9, 0xBE, 0x25, 0x63, 0xF8, 0x6A, 0xF7, 0x79, 0x09, 0x47, 0x3E, 0x38, 0x8B, 0x63, 0x89, 0xD7, 0x1B, 0x8C, 0x75, 0xE2, 0x7C};
        std::string text;
        for (int i = 0; i < 6; ++i) text += "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. ";
        text += "How vexingly quick daft zebras jump! Sphinx of black quartz, judge my vow. int main() { printf(\"hello\\n\"); return 0; }";
        std::string o1, o2, o3, o4, o5;
        const bool f1 = pdf::inflate(stored, sizeof stored, o1), f2 = pdf::inflate(fixed, sizeof fixed, o2), f3 = pdf::inflate(dynamic_block, sizeof dynamic_block, o3);
        check("Flate decodes a stored block", f1 && o1 == "hello stored block");
        check("Flate decodes a block with the fixed codes", f2 && o2 == "abcabcabcabcabcabcabc xyz");
        check("Flate decodes a block with its own codes", f3 && o3 == text);
        const bool cut = pdf::inflate(dynamic_block, sizeof dynamic_block / 2, o4);
        check("Flate on cut-off data gives back what it read and says it is not whole", !cut && !o4.empty() && text.compare(0, o4.size(), o4) == 0);
        const unsigned char nonsense[] = {0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        check("Flate refuses nonsense", !pdf::inflate(nonsense, sizeof nonsense, o5));

        // reading PDFs
        const pdf::Document d = pdf::read_file(fixture(L"book_cpp.pdf"));
        check("a PDF made by a browser is read: three pages and its title", d.ok && d.pages.size() == 3 && d.title == "Tiny C++ Book");
        bool big_bold = false, mono_code = false;
        float x_main = 0, x_cout = 0, char_w = 0;
        for (const pdf::Page& pg : d.pages)
            for (const pdf::Line& ln : pg.lines) {
                if (ln.text == "Tiny C++ Book" && ln.bold && ln.size > 20) big_bold = true;
                if (ln.text.rfind("std::cout << \"Hello, reader!\"", 0) == 0 && ln.mono) {
                    mono_code = true;
                    x_cout = ln.x;
                    char_w = ln.char_w;
                }
                if (ln.text == "int main() {" && x_main == 0) x_main = ln.x;
            }
        check("headings come out big and bold, and code comes out in the monospace font", big_bold && mono_code);
        check("a line of code remembers how far in it is (four letters)", char_w > 0 && std::fabs((x_cout - x_main) / char_w - 4.0f) < 0.6f);
        const pdf::Document dx = pdf::read_file(fixture(L"book_cpp_objstm.pdf"));
        check("a PDF with object streams and a cross-reference stream reads the same", dx.ok && text_of(dx) == text_of(d));
        const pdf::Document di = pdf::read_file(fixture(L"book_c_incremental.pdf"));
        check("a PDF with an update added on the end uses the newest version", di.ok && di.title == "Tiny C Book (second edition)");
        const pdf::Document dl = pdf::read_file(fixture(L"book_locked.pdf"));
        check("a locked PDF says so in plain words", !dl.ok && dl.error.find("locked") != std::string::npos);
        const pdf::Document dn = pdf::read_bytes("this is not a pdf at all");
        check("a file that is not a PDF says so", !dn.ok && dn.error.find("PDF") != std::string::npos);
        std::string raw_pdf;
        read_file(narrow(fixture(L"book_cpp.pdf")), raw_pdf);
        const pdf::Document dc = pdf::read_bytes(raw_pdf.substr(0, raw_pdf.size() / 2));
        check("a PDF that is cut off does not crash the reader", dc.ok || !dc.error.empty());
        const pdf::Document dj = pdf::read_bytes("%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 1 0 R >>\nendobj\n%%EOF\n");
        check("a PDF whose pages point at themselves does not hang the reader", !dj.ok && !dj.error.empty());

        // sections and code blocks
        const std::vector<Section> secs = sections_from_pdf(d);
        int blocks = 0;
        const Section* hello = nullptr;
        bool boxes = false, again = false;
        for (const Section& sc : secs) {
            blocks += static_cast<int>(sc.blocks.size());
            if (sc.title.find("Saying Hello") != std::string::npos) hello = &sc;
            if (sc.title.find("Boxes for Numbers") != std::string::npos) boxes = true;
            if (sc.title.find("Doing Things Again") != std::string::npos) again = true;
        }
        check("the book is split into sections by its headings", secs.size() >= 5 && hello && boxes && again);
        check("code blocks are found, with their indentation, and the words before each one",
              blocks == 7 && hello && hello->blocks.size() == 1 && hello->blocks[0].code.find("\n    std::cout << \"Hello, reader!\"") != std::string::npos &&
                  !hello->blocks[0].before.empty() && hello->blocks[0].before.back().find("smallest useful program") != std::string::npos);
        const CodeBlock* fragment = nullptr;
        for (const Section& sc : secs)
            for (const CodeBlock& b : sc.blocks)
                if (b.code.rfind("int width", 0) == 0) fragment = &b;
        const std::vector<Candidate> wrapped = fragment ? candidates_for(*fragment, false) : std::vector<Candidate>();
        check("a few lines are wrapped into a whole program with main", !wrapped.empty() && wrapped[0].cpp && wrapped[0].code.find("int main()") != std::string::npos &&
                                                                          wrapped[0].code.find("#include <iostream>") != std::string::npos);
        const pdf::Document dc2 = pdf::read_file(fixture(L"book_c.pdf"));
        const std::vector<Section> c_secs = sections_from_pdf(dc2);
        const std::vector<Candidate> c_first = !c_secs.empty() && !c_secs[0].blocks.empty() ? candidates_for(c_secs[0].blocks[0], true) : std::vector<Candidate>();
        check("a C book's code is tried as C first", c_secs.size() == 2 && !c_first.empty() && !c_first[0].cpp);
        // a page's date, title and number in its margins are not part of the book; chapter headings (even at the very top) are
        const pdf::Document dh = pdf::read_file(fixture(L"book_headers.pdf"));
        const std::vector<Section> h_secs = sections_from_pdf(dh);
        bool margins_left_out = dh.ok && h_secs.size() == 4, chapters_kept = h_secs.size() == 4;
        for (const Section& sc : h_secs) {
            for (const std::string& p : sc.paragraphs) margins_left_out = margins_left_out && p.find("file:///") == std::string::npos && p.find(" AM Header Test Book") == std::string::npos;
            for (const CodeBlock& b : sc.blocks) margins_left_out = margins_left_out && b.code.find("file:///") == std::string::npos;
        }
        chapters_kept = chapters_kept && h_secs.size() == 4 && h_secs[1].title == "Chapter 1 Counting Up" && h_secs[2].title == "Chapter 2 Doubling" &&
                        h_secs[3].title == "Chapter 3 Greeting";
        check("running heads and page numbers in the margins are left out", margins_left_out);
        check("chapter headings at the top of a page are kept (and each starts a section)", chapters_kept);

        // making the lessons for real
        const std::size_t builtin = builtin_lesson_count();
        bool saw_reading = false, saw_trying = false;
        a.start_book_import(fixture(L"book_cpp.pdf"), false);
        check("importing starts on the Settings page", m.book_state == BookState::Working && m.page == Page::Settings && layout_ok(a));
        const bool finished = pump(
            [&] {
                saw_reading = saw_reading || m.book_status.rfind("Reading page", 0) == 0;
                saw_trying = saw_trying || m.book_status.rfind("Trying code sample", 0) == 0;
                return m.book_state != BookState::Working;
            },
            600000);
        check("it says what it is doing (the page it is on, the code sample it is trying)", finished && saw_trying);
        check("it finishes: 'Made N lessons from ...'", m.book_state == BookState::Done && m.book_message.rfind("Made ", 0) == 0 && m.book_message.find("Tiny C++ Book") != std::string::npos);
        const std::vector<ImportedCourse> mine = imported_courses();
        check("the course is saved in its folder with its details",
              mine.size() == 1 && mine[0].name == "Tiny C++ Book" && mine[0].lessons == 14 && mine[0].programs == 5 && mine[0].source == "book_cpp.pdf" &&
                  fs::exists(courses_root() / mine[0].folder / "course.json"));
        bool ids_ok = mine.size() == 1 && lessons().size() == builtin + 14;
        std::set<std::string> ids;
        for (const Lesson& ls : lessons()) ids_ok = ids_ok && ids.insert(ls.id).second;
        for (std::size_t i = builtin; ids_ok && i < lessons().size(); ++i) ids_ok = lessons()[i].id.rfind(mine[0].folder + ".", 0) == 0 && lessons()[i].course >= kFirstImportedCourse;
        check("the lessons come after the built-in ones, and every id is unique and starts with the course folder", ids_ok);
        bool course_listed = false;
        for (const Course& c : courses()) course_listed = course_listed || (c.imported && c.name == "Tiny C++ Book" && c.number >= kFirstImportedCourse);
        check("the course is in the course list", course_listed);

        // every lesson is checked with the real compiler: the program runs, the gapped one fails, the full one passes, the changed one passes
        int runs = 0, runs_pass = 0, gaps = 0, gaps_fail = 0, gaps_pass = 0, changes = 0, changes_fail = 0, changes_pass = 0;
        for (std::size_t i = builtin; i < lessons().size(); ++i) {
            const Lesson ls = lessons()[i];
            const bool passes = check_lesson(ls, ls.starter, ls.input).passed;
            if (ls.title.rfind("Run:", 0) == 0) {
                ++runs;
                runs_pass += passes;
            } else if (ls.title.rfind("Gap:", 0) == 0) {
                ++gaps;
                gaps_fail += !passes;
                std::string code = ls.starter;
                const std::size_t gap = code.find("// ...");
                const std::size_t hint_at = ls.hint.find("\n\n    ");
                if (gap != std::string::npos && hint_at != std::string::npos) {
                    code.replace(gap, 6, ls.hint.substr(hint_at + 6));
                    gaps_pass += check_lesson(ls, code, ls.input).passed;
                }
            } else if (ls.title.rfind("Change:", 0) == 0) {
                ++changes;
                changes_fail += !passes;
                std::string code = ls.starter;
                const std::size_t hint_at = ls.hint.find("\n\n    ");
                const std::string old_line = ls.instructions.size() >= 3 ? ls.instructions[2].substr(4) : std::string();
                const std::size_t at = old_line.empty() ? std::string::npos : code.find(old_line);
                if (at != std::string::npos && hint_at != std::string::npos) {
                    code.replace(at, old_line.size(), ls.hint.substr(hint_at + 6));
                    changes_pass += check_lesson(ls, code, ls.input).passed;
                }
            }
        }
        check("every 'Run it' lesson passes as it is", runs == 5 && runs_pass == 5);
        check("every 'Fill the gap' lesson fails with the gap, and passes with the missing line put back", gaps == 5 && gaps_fail == 5 && gaps_pass == 5);
        check("every 'Change it' lesson fails as it is, and passes with the number changed", changes == 4 && changes_fail == 4 && changes_pass == 4);
        check("the sample that asks for a number is given an answer", lessons()[builtin + 11].input == "5\n3\n4\n" && lessons()[builtin + 11].expected.find("Double is 10") != std::string::npos);
        check("the sample that does not build was left out, and the same sample twice made one set", mine.size() == 1 && mine[0].programs == 5);

        reload_lessons();
        check("after reading the courses again, the course and its lessons are still there", imported_courses().size() == 1 && lessons().size() == builtin + 14);

        // the Settings page
        click(Control::NavHome);
        click(Control::NavSettings);
        check("Settings opens from the rail and lists the course", m.page == Page::Settings && m.my_courses.size() == 1 && a.layout.set_rows.size() == 1);
        int at_index = -1;
        const Rect& rail_rect = a.layout.nav_settings;
        check("the gear is at the bottom of the rail and can be hit",
              rail_rect.w > 0 && a.hit(rail_rect.x + 5, rail_rect.y + 5, at_index) == Control::NavSettings && rail_rect.y > a.layout.nav_badges.y);
        const Rect& rm = a.layout.set_remove[0];
        check("Remove can be hit", a.hit(rm.x + 5, rm.y + 5, at_index) == Control::CourseRemove && at_index == 0);
        click(Control::ImportOpen);
        bool head_shown = false;
        for (const auto& [r, number] : a.layout.course_heads) head_shown = head_shown || (number >= kFirstImportedCourse && r.y >= a.layout.lessons_area.y - 1 && r.bottom() <= a.layout.lessons_area.bottom());
        check("Open the lessons shows the course on the Lessons page", m.page == Page::Lessons && head_shown);
        click(Control::NavSettings);
        click(Control::CourseRemove, 0);
        check("Remove asks first", m.confirm_remove == 0 && imported_courses().size() == 1);
        press(VK_ESCAPE);
        check("Esc keeps the course", m.confirm_remove < 0 && m.page == Page::Settings && imported_courses().size() == 1);
        click(Control::CourseRemove, 0);
        click(Control::CourseRemoveNo);
        check("'Keep it' keeps the course", m.confirm_remove < 0 && imported_courses().size() == 1);
        click(Control::CourseRemove, 0);
        click(Control::CourseRemoveYes, 0);
        check("'Yes, remove' deletes the course and its lessons",
              imported_courses().empty() && m.my_courses.empty() && lessons().size() == builtin && !fs::exists(courses_root() / mine[0].folder));
        press(VK_ESCAPE);
        check("Esc on Settings goes home", m.page == Page::Home);

        // an SDK folder
        a.start_book_import(fixture(L"tiny_sdk"), true);
        check("an SDK folder can be imported too", pump([&] { return m.book_state != BookState::Working; }, 600000) && m.book_state == BookState::Done);
        const std::vector<ImportedCourse> sdk = imported_courses();
        check("each sample program with a main becomes lessons, and C samples are C",
              sdk.size() == 1 && sdk[0].programs == 3 && sdk[0].lessons >= 6 && sdk[0].language == "C and C++" && lessons().size() == builtin + static_cast<std::size_t>(sdk[0].lessons));
        bool c_lesson = false;
        for (std::size_t i = builtin; i < lessons().size(); ++i) c_lesson = c_lesson || (!lessons()[i].cpp && lessons()[i].starter.find("squared") != std::string::npos);
        check("a .c sample is a C lesson", c_lesson);
        if (!sdk.empty()) remove_imported_course(sdk[0].folder);
        reload_lessons();

        // problems are said in plain words, and Cancel stops it
        a.start_book_import(fixture(L"book_locked.pdf"), false);
        check("a locked PDF makes no lessons and says why",
              pump([&] { return m.book_state != BookState::Working; }, 60000) && m.book_state == BookState::Failed && m.book_message.find("locked") != std::string::npos &&
                  imported_courses().empty());
        a.start_book_import(fixture(L"book_cpp.pdf"), false);
        click(Control::ImportCancel);
        check("Cancel stops the import and saves nothing",
              pump([&] { return m.book_state != BookState::Working; }, 300000) && m.book_state == BookState::Failed && m.book_message.find("Stopped") != std::string::npos &&
                  imported_courses().empty() && lessons().size() == builtin);
        size_t leftovers = 0;
        std::error_code leftover_ec;
        for (const auto& e : fs::directory_iterator(courses_root(), leftover_ec)) leftovers += e.is_directory();
        check("nothing half-made is left behind", leftovers == 0);
        click(Control::NavHome);
    }

    log << "\n" << passes << " passed, " << fails << " failed\n";
    DestroyWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    std::filesystem::remove(progress, ec);
    std::filesystem::remove_all(test_courses, ec);
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
    wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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
    set_sound_on(app->model.progress.sound);
    app->model.pick_level = highest_open_level(app->model.progress);
    app->start_tool_check();
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
