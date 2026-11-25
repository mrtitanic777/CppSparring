#pragma once
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "compile.hpp"
#include "lessons.hpp"

namespace sparring {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

constexpr float kTitleBarHeight = 48.0f;
constexpr float kOuterPadding = 24.0f;
constexpr float kCardGap = 16.0f;
constexpr float kCardRadius = 8.0f;
constexpr float kStatusBarHeight = 32.0f;
constexpr float kCaptionButtonWidth = 46.0f;
constexpr float kRailWidth = 72.0f;
constexpr float kCodeLineHeight = 20.0f;
constexpr float kLookRowHeight = 108.0f;

// The places (Home, Practice, Lessons, Badges) and what opens from them: a program's Look and Break steps, one lesson, and the
// quest's own pages. The rail only ever lists places.
enum class Page { Home, Practice, Look, Break, Badges, Lessons, Lesson, Quiz, QuestDone };

enum class Control {
    None, CaptionMinimize, CaptionMaximize, CaptionClose,
    NavHome, NavPractice, NavLearn, NavBadges,
    HeroQuest, DoorPractice, DoorLessons, DoorBadges,
    SessionBack, StepLook, StepBreak,
    LevelTab, Example, OpenFile,
    CodeView, LookRow, LookList, NextStep, Watch,
    ReplayBack, ReplayPlay, ReplayNext, ReplayBar, ReplayStop,
    PrevChallenge, NextChallenge, Guess, NextAfterResult, WatchChanged, BossTimer,
    QuestSkip, QuestQuit,
    LessonTile, LessonList, LessonRun, LessonReset, LessonHint, LessonNext, Editor,
    QuizAnswer, QuizNext,
    ChestOpen, QuestHome, QuestAgain
};

struct AstNode {
    std::string id;        // clang's node id, for declarations
    std::string ref_id;    // the declaration a DeclRefExpr / MemberExpr names
    std::string name;
    std::string kind;
    std::string type;
    int line = 0;
    int end_line = 0;        // the last line of a job (function)
    int scope_end = 0;       // a variable: the last line it can be used on (huge for globals)
    std::string function;    // the job it is inside ("" outside any)
    std::vector<int> dependencies;
};

enum class Rule { RemoveRef, RemoveConst, RemoveReturn, RemoveIgnore, EqToAssign, SwapOp, RemoveAssign };

// One way to break the program.
struct MutationToggle {
    Rule rule = Rule::RemoveRef;
    std::string label;          // "Take away the & from bank"
    std::string description;    // why that matters, in plain words
    std::string name;
    std::string op_from, op_to; // SwapOp: "+" -> "-"
    int line = 0;
    int target_node = -1;
};

// The four answers to "What do you think will happen?" (the order of the buttons).
enum class Outcome { WontBuild = 0, Crashes = 1, Different = 2, Same = 3 };

struct Challenge {
    int guess = -1;             // an Outcome, -1 before guessing
    bool ran = false;
    int truth = -1;             // an Outcome, -1 when it cannot be told (the original does not build)
    CompileResult result;
};

// One thing in the program, said in plain words for the Look step.
enum class LookKind { Start, Job, Blueprint, Box };
struct LookItem {
    LookKind kind = LookKind::Job;
    std::string name;
    std::string sentence;
    std::string detail;
    int line = 0;
};

// ---- watching a program run, one line at a time (replay.hpp makes these)
struct ReplayVar {
    std::string name;
    std::string value;
};
struct ReplayStep {
    int line = 0;
    std::string function;
    std::vector<ReplayVar> vars;
    std::string printed;   // everything printed up to this step
};
struct Replay {
    bool ok = false;
    std::string error;
    std::string source;    // the program that was watched
    std::vector<ReplayStep> steps;
    std::string output;    // everything it printed
    bool crashed = false;
    bool timed_out = false;
    bool cut_short = false;
};

// ---- what the learner has done, kept between runs (progress.hpp)
struct Progress {
    std::set<std::string> right;              // "file|challenge label@line": guessed right at least once
    std::map<std::string, int> totals;        // file -> how many challenges it has
    std::set<std::string> badges;             // earned badge ids
    int streak = 0, best_streak = 0;
    int guesses = 0, correct = 0;
    std::set<std::string> lessons_done;       // lesson ids passed
    std::set<std::string> quiz_right;         // quiz card ids answered right
    std::set<std::string> days;               // "2026-11-24": days something was done
    int xp = 0;
    int quests_done = 0;
    int bosses_beaten = 0;
};

// Something to cheer about: a badge, or a level that just opened.
struct Cheer {
    std::string title;
    std::string sub;
    std::wstring mark;
    bool glyph = true;
};

// ---- the Daily Quest: a few short, mixed steps with a clear end (quest.hpp makes them)
enum class StepKind { Quiz, Guess, Write };
struct QuestStep {
    StepKind kind = StepKind::Quiz;
    int quiz = -1;        // Quiz: a card of quiz_cards()
    int example = -1;     // Guess: an example program
    int challenge = -1;   // Guess: which challenge, chosen once the program is read
    int lesson = -1;      // Write: a lesson of lessons()
    bool done = false, skipped = false, right = false;
};
struct Quest {
    bool active = false;
    bool boss = false;            // a Bug Boss: every guess right hits it
    bool finished = false;
    std::vector<QuestStep> steps;
    int at = 0;
    int boss_hp = 0, boss_max = 0;
    int combo = 0, best_combo = 0;
    int xp = 0;                   // earned in this quest
    bool timer = false;           // Bug Boss only, and only when switched on
    float time_left = 0;
    int chest_xp = 0;
    bool chest_open = false;
    std::set<std::string> used;   // "example#challenge" already in this quest
};

struct Model {
    Page page = Page::Home;
    Control hot = Control::None;
    Control pressed = Control::None;
    int hot_index = -1;
    int pressed_index = -1;
    bool maximized = false;
    bool active = true;

    Progress progress;
    int pick_level = 1;

    std::string source_path;
    std::string source_name;
    std::string source_code;
    int code_scroll = 0;
    int highlight_line = 0;
    bool importing = false;
    std::string import_error;

    std::vector<AstNode> nodes;
    std::vector<LookItem> look;
    int look_scroll = 0;

    std::vector<MutationToggle> toggles;
    std::vector<Challenge> challenges;
    int current = 0;
    bool running = false;
    bool original_ready = false;
    CompileResult original;

    // watching it run
    bool replay_open = false;
    bool replay_loading = false;
    bool replay_playing = false;
    int replay_step = 0;
    std::string replay_title;
    Page replay_from = Page::Look;
    Replay replay;

    // cheering: the first one is showing
    std::vector<Cheer> cheers;
    float cheer_t = -1.0f;

    // the Daily Quest
    Quest quest;

    // the write-it lessons
    int lessons_scroll = 0;
    int lesson = -1;                 // the open lesson, an index of lessons()
    std::string lesson_code;         // what is in the editor
    bool lesson_running = false;
    bool lesson_ran = false;
    bool hint_shown = false;
    LessonCheck lesson_check;
    bool native_editor = false;      // the real edit box is on screen (not in a screenshot)

    // a quiz card
    int quiz = -1;
    int quiz_choice = -1;            // the shown position picked, -1 before answering

    // animation, each 0..1 (1: finished); confetti in seconds since it started, < 0 when there is none
    float page_anim = 1.0f;
    float result_anim = 1.0f;
    float hover_anim = 1.0f;
    float step_anim = 1.0f;
    float confetti = -1.0f;

    int score() const {
        int n = 0;
        for (const Challenge& c : challenges) n += c.ran && c.truth >= 0 && c.guess == c.truth;
        return n;
    }
    int answered() const {
        int n = 0;
        for (const Challenge& c : challenges) n += c.ran && c.truth >= 0;
        return n;
    }
};

struct Layout {
    float width = 0, height = 0;

    Rect title_bar, app_icon, title_text;
    Rect caption_minimize, caption_maximize, caption_close;

    bool focus = false;   // a quest is running: no rail and no status bar, just the step
    Rect rail, nav_home, nav_practice, nav_learn, nav_badges;
    Rect content;
    // the quest's bar over the page while a quest runs
    Rect quest_bar, quest_skip, quest_quit;
    // inside a program or a lesson (not in a quest): back, where you are, and the program's Look / Break steps
    Rect session_bar, session_back, step_look, step_break;
    Rect status_bar, status_dot, status_text, status_right;
    Rect toast;

    Rect card_a, card_b;

    // Home: the big button, and three doors
    Rect hero, hero_button, hero_side;
    std::array<Rect, 3> doors{};
    // Practice: the programs by level
    Rect examples_label, tiles_area;
    std::array<Rect, 4> level_tabs{};
    std::vector<Rect> examples;
    std::vector<std::size_t> example_items;   // which example each tile shows
    Rect drop_area, open_button;

    // Look
    Rect code_view, look_list, next_step, watch_button;
    std::vector<Rect> rows;
    int look_first = 0;
    int look_hidden = 0;
    // watching it run (in the Look page's right card)
    Rect replay_back, replay_play, replay_next, replay_count, replay_bar, replay_vars, replay_console, replay_stop;

    // Break
    Rect prev_button, next_button;
    Rect before_box, after_box, why_text, question;
    std::array<Rect, 4> guesses{};
    Rect banner, result_text, real_life, out_before, out_after, next_challenge, watch_changed;
    Rect boss_hud, boss_timer;

    // Badges
    std::array<Rect, 4> stats{};
    std::vector<Rect> badge_tiles;

    // Lessons: the list, with a heading per course
    Rect lessons_area;
    std::vector<Rect> lesson_tiles;
    std::vector<int> lesson_items;
    std::vector<std::pair<Rect, int>> course_heads;   // heading rect, course number
    float lessons_height = 0;                          // the whole list, for scrolling

    // Lesson: read on the left, write and run on the right
    Rect lesson_text, task_box, expect_box, hint_box, editor, run_button, reset_button, hint_button, lesson_out, lesson_banner, lesson_next;

    // Quiz
    Rect quiz_card, quiz_code;
    std::array<Rect, 4> quiz_answers{};
    Rect quiz_why, quiz_next;

    // Quest done
    Rect done_card, chest, done_home, done_again;
};

Layout compute_layout(const Model& m, float width, float height);

}  // namespace sparring
