#pragma once
#include <string>

namespace sparring {

// `file`: a C++ file to open at once (may be empty).
int run_window(const std::wstring& file);

// Draws the window into a PNG without showing it.
struct ShotOptions {
    std::wstring out_png;
    std::wstring file;             // a .cpp, or "example:N"
    std::wstring page = L"home";   // home | practice | look | break | lessons | lesson | quiz | questdone | badges
    int guess = -1;                // >= 0: answer the first challenge with this and really run it
    int replay_step = -1;          // >= 0: watch it run, stopped at this step
    int level = 0;                 // the Pick page's level tab (0: the highest open one)
    bool demo = false;             // pretend some progress was made (badges, stars, a cheer)
    bool dark = false;
    int lesson = -1;               // page lesson: which one; with --run it is really built and checked
    bool run_lesson = false;
    bool hint = false;
    int quiz = -1;                 // page quiz: which card; --guess picks an answer
    int quest_step = -1;           // >= 0: a quest is under way at this step
    bool boss = false;             // the quest is a Bug Boss
    bool chest = false;            // page questdone: the chest is open
};
int screenshot(const ShotOptions& o);

}  // namespace sparring
