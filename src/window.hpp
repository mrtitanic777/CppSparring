#pragma once
#include <string>

namespace sparring {

// `file`: a C++ file to open at once (may be empty).
int run_window(const std::wstring& file);

// Draws the window into a PNG without showing it.
struct ShotOptions {
    std::wstring out_png;
    std::wstring file;             // a .cpp, or "example:N"
    std::wstring page = L"home";   // home | practice | look | break | fix | lessons | lesson | quiz | questdone | badges | summary | setup
    int guess = -1;                // >= 0: answer the first challenge with this and really run it
    int replay_step = -1;          // >= 0: watch it run, stopped at this step
    int level = 0;                 // the Pick page's level tab (0: the highest open one)
    bool demo = false;             // pretend some progress was made (badges, stars, a cheer)
    bool dark = false;
    int lesson = -1;               // page lesson: which one; with --run it is really built and checked
    bool run_lesson = false;
    int buddy = 0;                 // page lesson: Bit is there (1 hello, 2 pointing at the hint, 3 explaining a block, 4 listening)
    std::wstring code_file;        // page lesson: the code in the editor (else the starter)
    bool hint = false;
    int quiz = -1;                 // page quiz: which card; --guess picks an answer
    int quest_step = -1;           // >= 0: a quest is under way at this step
    bool boss = false;             // the quest is a Bug Boss
    bool chest = false;            // page questdone: the chest is open
    int setup = 0;                 // page setup: 0 nothing installed, 1 Visual Studio but no Clang, 2 all set
};
int screenshot(const ShotOptions& o);

// Clicks through every part of the real window, never shown, and writes what passed and failed to `log_path`.
// Uses a throwaway progress file. Returns the number of failures.
int self_test(const std::wstring& log_path);

// Draws the app icon and saves it as an .ico (src/cppsparring.ico is made with this).
int make_icon(const std::wstring& out_path);

// Adds CppSparring to the "Open with" list for .cpp files (for this Windows user only; the default program is not changed).
int register_open_with();

}  // namespace sparring
