#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// What a learner has done, kept between runs in %APPDATA%\CppSparring\progress.txt.

struct Badge {
    const char* id;
    const char* name;
    const char* how;        // how to earn it, in plain words
    const wchar_t* mark;    // a Segoe Fluent Icons glyph, or text when `glyph` is false
    bool glyph;
};

const std::vector<Badge>& all_badges();
const Badge* find_badge(const std::string& id);

// Use another file than %APPDATA%\CppSparring\progress.txt (the self-test keeps the real one untouched).
void use_progress_file(const std::wstring& path);

Progress load_progress();
void save_progress(const Progress& p);

int stars_for(const Progress& p, const std::string& file);   // 0..3
int total_stars(const Progress& p);                          // over the examples
bool level_open(const Progress& p, int level);
int highest_open_level(const Progress& p);
constexpr int kRightToOpenNextLevel = 2;
int right_in_level(const Progress& p, int level);

// A guess was checked. Updates the counts and returns what was just earned. `bonus`: extra XP (a Bug Boss combo).
std::vector<Cheer> record_guess(Progress& p, const std::string& file, const MutationToggle& t, bool right, Outcome truth, int bonus = 0);
// The learner watched a program run step by step.
std::vector<Cheer> record_watch(Progress& p);
// A lesson's code was run; passed or not.
std::vector<Cheer> record_lesson(Progress& p, const std::string& id, bool passed);
// A Fix it puzzle was run; fixed or not.
std::vector<Cheer> record_fix(Progress& p, const std::string& id, bool passed);

// A quiz card was answered.
std::vector<Cheer> record_quiz(Progress& p, const std::string& id, bool right);
// A quest was finished (a Bug Boss quest, beaten or not), with the treasure chest's XP.
std::vector<Cheer> record_quest(Progress& p, bool boss, bool beaten, int chest_xp);

// ---- XP and ranks
struct Rank {
    const char* name;
    int xp;   // XP needed
};
const std::vector<Rank>& ranks();
std::size_t rank_index(int xp);

// ---- the gentle weekly goal: days played this week (Monday to Sunday), no punishment for missing one
constexpr int kWeekGoal = 4;
std::string today();
std::vector<std::string> this_week();   // Monday..Sunday as "YYYY-MM-DD"
int days_this_week(const Progress& p);

// What each thing is worth.
constexpr int kXpRight = 10, kXpTried = 3, kXpLesson = 15, kXpLessonAgain = 5, kXpQuizRight = 5, kXpQuizTried = 1, kXpFix = 15, kXpQuest = 20, kXpBoss = 50;

}  // namespace sparring
