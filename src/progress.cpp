#include "progress.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "examples.hpp"

namespace sparring {

namespace {

std::filesystem::path progress_file() {
    PWSTR base = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) dir = std::filesystem::path(base) / L"CppSparring";
    CoTaskMemFree(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"progress.txt";
}

std::string key(const std::string& file, const MutationToggle& t) { return file + "|" + t.label + "@" + std::to_string(t.line); }

std::string file_of(const std::string& k) { return k.substr(0, k.find('|')); }

Cheer badge_cheer(const Badge& b) { return Cheer{std::string("Badge: ") + b.name, b.how, b.mark, b.glyph}; }

void earn(Progress& p, std::vector<Cheer>& out, const char* id) {
    if (!p.badges.insert(id).second) return;
    if (const Badge* b = find_badge(id)) out.push_back(badge_cheer(*b));
}

void add_xp(Progress& p, std::vector<Cheer>& out, int n) {
    const std::size_t before = rank_index(p.xp);
    p.xp += n;
    const std::size_t after = rank_index(p.xp);
    if (after > before) {
        const Rank& r = ranks()[after];
        out.push_back(Cheer{std::string("New rank: ") + r.name + "!", std::to_string(p.xp) + " XP and climbing.", L"\xE7C1", true});
    }
}

// Today counts as a day played; reaching the week's goal earns its badge.
void played_today(Progress& p, std::vector<Cheer>& out) {
    const bool had_goal = days_this_week(p) >= kWeekGoal;
    p.days.insert(today());
    if (!had_goal && days_this_week(p) >= kWeekGoal) {
        out.push_back(Cheer{"Weekly goal reached!", std::to_string(kWeekGoal) + " days this week. Your brain says thanks.", L"\xE787", true});
        earn(p, out, "week");
    }
}

std::string date_string(const SYSTEMTIME& t) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return buf;
}

std::string shifted(const SYSTEMTIME& base, int days) {
    FILETIME ft{};
    SystemTimeToFileTime(&base, &ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<long long>(days) * 864000000000LL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME out{};
    FileTimeToSystemTime(&ft, &out);
    return date_string(out);
}

}  // namespace

const std::vector<Badge>& all_badges() {
    static const std::vector<Badge> all = {
        {"first", "First guess", "Make your very first guess.", L"\xE7C1", true},
        {"right", "Got one!", "Guess right for the first time.", L"\xE73E", true},
        {"streak3", "On a roll", "Guess right 3 times in a row.", L"\xE945", true},
        {"streak5", "Unstoppable", "Guess right 5 times in a row.", L"5", false},
        {"ref", "Reference ranger", "Guess right on a challenge about &.", L"&", false},
        {"const", "Promise keeper", "Guess right on a challenge about const.", L"\xE72E", true},
        {"crash", "Crash detective", "Guess right that a program crashes or gets stuck.", L"!", false},
        {"same", "Spot the twin", "Guess right that nothing changes.", L"=", false},
        {"watch", "Watcher", "Watch a program run step by step.", L"\xE890", true},
        {"perfect", "Perfect score", "Get all 3 stars on a program.", L"\xE735", true},
        {"level2", "Level up", "Open Level 2.", L"2", false},
        {"level4", "Blueprint builder", "Open Level 4.", L"4", false},
        {"quest1", "Quester", "Finish a Daily Quest.", L"\xE7C1", true},
        {"boss", "Boss beater", "Beat a Bug Boss.", L"\xEBE8", true},
        {"lesson1", "Code writer", "Pass your first Write-it lesson.", L"\xE70F", true},
        {"lessons10", "Ten lessons", "Pass 10 Write-it lessons.", L"10", false},
        {"quiz5", "Quick thinker", "Get 5 quiz cards right.", L"?", false},
        {"week", "Week warrior", "Play on 4 days in one week.", L"\xE787", true},
    };
    return all;
}

const Badge* find_badge(const std::string& id) {
    for (const Badge& b : all_badges())
        if (id == b.id) return &b;
    return nullptr;
}

const std::vector<Rank>& ranks() {
    static const std::vector<Rank> all = {{"Rookie", 0},          {"Bug Spotter", 100},  {"Code Detective", 250},
                                          {"Bug Hunter", 500},    {"Code Wizard", 1000}, {"Master Breaker", 2000}};
    return all;
}

std::size_t rank_index(int xp) {
    std::size_t r = 0;
    for (std::size_t i = 0; i < ranks().size(); ++i)
        if (xp >= ranks()[i].xp) r = i;
    return r;
}

std::string today() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return date_string(t);
}

std::vector<std::string> this_week() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const int since_monday = (t.wDayOfWeek + 6) % 7;
    std::vector<std::string> out;
    for (int i = 0; i < 7; ++i) out.push_back(shifted(t, i - since_monday));
    return out;
}

int days_this_week(const Progress& p) {
    int n = 0;
    for (const std::string& d : this_week()) n += p.days.count(d) > 0;
    return n;
}

Progress load_progress() {
    Progress p;
    std::ifstream f(progress_file());
    std::string line;
    while (std::getline(f, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "right") p.right.insert(v);
        else if (k == "badge") p.badges.insert(v);
        else if (k == "lesson") p.lessons_done.insert(v);
        else if (k == "quiz") p.quiz_right.insert(v);
        else if (k == "day") p.days.insert(v);
        else if (k == "streak") p.streak = std::atoi(v.c_str());
        else if (k == "best_streak") p.best_streak = std::atoi(v.c_str());
        else if (k == "guesses") p.guesses = std::atoi(v.c_str());
        else if (k == "correct") p.correct = std::atoi(v.c_str());
        else if (k == "xp") p.xp = std::atoi(v.c_str());
        else if (k == "quests") p.quests_done = std::atoi(v.c_str());
        else if (k == "bosses") p.bosses_beaten = std::atoi(v.c_str());
        else if (k == "total") {
            const std::size_t bar = v.rfind('|');
            if (bar != std::string::npos) p.totals[v.substr(0, bar)] = std::atoi(v.c_str() + bar + 1);
        }
    }
    return p;
}

void save_progress(const Progress& p) {
    std::ostringstream s;
    s << "streak=" << p.streak << "\nbest_streak=" << p.best_streak << "\nguesses=" << p.guesses << "\ncorrect=" << p.correct << "\nxp=" << p.xp
      << "\nquests=" << p.quests_done << "\nbosses=" << p.bosses_beaten << "\n";
    for (const auto& [file, n] : p.totals) s << "total=" << file << "|" << n << "\n";
    for (const std::string& r : p.right) s << "right=" << r << "\n";
    for (const std::string& b : p.badges) s << "badge=" << b << "\n";
    for (const std::string& l : p.lessons_done) s << "lesson=" << l << "\n";
    for (const std::string& q : p.quiz_right) s << "quiz=" << q << "\n";
    for (const std::string& d : p.days) s << "day=" << d << "\n";
    std::ofstream(progress_file(), std::ios::binary | std::ios::trunc) << s.str();
}

int stars_for(const Progress& p, const std::string& file) {
    const auto it = p.totals.find(file);
    if (it == p.totals.end() || it->second <= 0) return 0;
    int right = 0;
    for (const std::string& r : p.right) right += file_of(r) == file;
    const double share = static_cast<double>(right) / it->second;
    if (share >= 0.999) return 3;
    if (share >= 0.5) return 2;
    return right > 0 ? 1 : 0;
}

int total_stars(const Progress& p) {
    int n = 0;
    for (const Example& e : examples()) n += stars_for(p, e.file);
    return n;
}

int right_in_level(const Progress& p, int level) {
    int n = 0;
    for (std::size_t i : examples_in_level(level))
        for (const std::string& r : p.right) n += file_of(r) == examples()[i].file;
    return n;
}

bool level_open(const Progress& p, int level) {
    if (level <= 1) return true;
    return level_open(p, level - 1) && right_in_level(p, level - 1) >= kRightToOpenNextLevel;
}

int highest_open_level(const Progress& p) {
    int best = 1;
    for (const Level& l : levels())
        if (level_open(p, l.number)) best = l.number;
    return best;
}

std::vector<Cheer> record_guess(Progress& p, const std::string& file, const MutationToggle& t, bool right, Outcome truth, int bonus) {
    std::vector<Cheer> out;
    std::vector<bool> was_open;
    for (const Level& l : levels()) was_open.push_back(level_open(p, l.number));
    const int stars_before = stars_for(p, file);

    ++p.guesses;
    played_today(p, out);
    earn(p, out, "first");
    if (right) {
        ++p.correct;
        ++p.streak;
        p.best_streak = std::max(p.best_streak, p.streak);
        p.right.insert(key(file, t));
        earn(p, out, "right");
        if (p.streak >= 3) earn(p, out, "streak3");
        if (p.streak >= 5) earn(p, out, "streak5");
        if (t.rule == Rule::RemoveRef) earn(p, out, "ref");
        if (t.rule == Rule::RemoveConst) earn(p, out, "const");
        if (truth == Outcome::Crashes) earn(p, out, "crash");
        if (truth == Outcome::Same) earn(p, out, "same");
        if (stars_for(p, file) == 3 && stars_before < 3) earn(p, out, "perfect");
        add_xp(p, out, kXpRight + bonus);
    } else {
        p.streak = 0;
        add_xp(p, out, kXpTried);
    }

    for (std::size_t i = 0; i < levels().size(); ++i) {
        const Level& l = levels()[i];
        if (was_open[i] || !level_open(p, l.number)) continue;
        out.push_back(Cheer{"Level " + std::to_string(l.number) + " is open!", std::string("New programs about ") + l.name + ". Go to Home to try them.",
                            std::to_wstring(l.number), false});
        if (l.number == 2) earn(p, out, "level2");
        if (l.number == 4) earn(p, out, "level4");
    }
    save_progress(p);
    return out;
}

std::vector<Cheer> record_watch(Progress& p) {
    std::vector<Cheer> out;
    earn(p, out, "watch");
    if (!out.empty()) save_progress(p);
    return out;
}

std::vector<Cheer> record_lesson(Progress& p, const std::string& id, bool passed) {
    std::vector<Cheer> out;
    played_today(p, out);
    if (passed) {
        const bool first_time = p.lessons_done.insert(id).second;
        add_xp(p, out, first_time ? kXpLesson : kXpLessonAgain);
        earn(p, out, "lesson1");
        if (p.lessons_done.size() >= 10) earn(p, out, "lessons10");
    }
    save_progress(p);
    return out;
}

std::vector<Cheer> record_quiz(Progress& p, const std::string& id, bool right) {
    std::vector<Cheer> out;
    played_today(p, out);
    if (right) {
        p.quiz_right.insert(id);
        if (p.quiz_right.size() >= 5) earn(p, out, "quiz5");
    }
    add_xp(p, out, right ? kXpQuizRight : kXpQuizTried);
    save_progress(p);
    return out;
}

std::vector<Cheer> record_quest(Progress& p, bool boss, bool beaten, int chest_xp) {
    std::vector<Cheer> out;
    played_today(p, out);
    ++p.quests_done;
    earn(p, out, "quest1");
    int xp = kXpQuest + chest_xp;
    if (boss && beaten) {
        ++p.bosses_beaten;
        earn(p, out, "boss");
        xp += kXpBoss;
    }
    add_xp(p, out, xp);
    save_progress(p);
    return out;
}

}  // namespace sparring
