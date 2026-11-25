#include "quest.hpp"

#include <algorithm>
#include <cstdint>

#include "examples.hpp"
#include "lessons.hpp"
#include "progress.hpp"
#include "quiz.hpp"

namespace sparring {

namespace {

struct Dice {
    std::uint32_t s;
    int below(int n) {
        s = s * 1664525u + 1013904223u;
        return n <= 0 ? 0 : static_cast<int>((s >> 8) % static_cast<std::uint32_t>(n));
    }
};

std::uint32_t seed_for(const Progress& p) {
    std::uint32_t h = 2166136261u;
    for (char c : today()) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h ^ (static_cast<std::uint32_t>(p.quests_done) * 2654435761u) ^ static_cast<std::uint32_t>(p.xp);
}

int pick_example(const Progress& p, Dice& dice, int avoid) {
    std::vector<int> fresh, all;
    for (std::size_t i = 0; i < examples().size(); ++i) {
        if (!level_open(p, examples()[i].level)) continue;
        all.push_back(static_cast<int>(i));
        if (stars_for(p, examples()[i].file) < 3 && static_cast<int>(i) != avoid) fresh.push_back(static_cast<int>(i));
    }
    const std::vector<int>& from = fresh.empty() ? all : fresh;
    return from.empty() ? 0 : from[static_cast<std::size_t>(dice.below(static_cast<int>(from.size())))];
}

int pick_quiz(const Progress& p, Dice& dice, int avoid) {
    std::vector<int> fresh, all;
    for (std::size_t i = 0; i < quiz_cards().size(); ++i) {
        if (static_cast<int>(i) == avoid) continue;
        all.push_back(static_cast<int>(i));
        if (!p.quiz_right.count(quiz_cards()[i].id)) fresh.push_back(static_cast<int>(i));
    }
    const std::vector<int>& from = fresh.empty() ? all : fresh;
    return from.empty() ? 0 : from[static_cast<std::size_t>(dice.below(static_cast<int>(from.size())))];
}

int next_lesson(const Progress& p, Dice& dice) {
    const auto& all = lessons();
    if (all.empty()) return -1;
    for (std::size_t i = 0; i < all.size(); ++i)
        if (!p.lessons_done.count(all[i].id)) return static_cast<int>(i);
    return dice.below(static_cast<int>(all.size()));
}

}  // namespace

int quests_until_boss(const Progress& p) { return (kQuestsPerBoss - 1) - p.quests_done % kQuestsPerBoss; }

Quest make_quest(const Progress& p) {
    Dice dice{seed_for(p)};
    Quest q;
    q.active = true;
    q.boss = quests_until_boss(p) == 0;
    q.chest_xp = 10 + dice.below(41);
    if (q.boss) {
        q.boss_hp = q.boss_max = kBossHp;
        int last = -1;
        for (int i = 0; i < 5; ++i) {
            QuestStep s;
            s.kind = StepKind::Guess;
            s.example = last = pick_example(p, dice, last);
            q.steps.push_back(s);
        }
        return q;
    }
    QuestStep quiz1{StepKind::Quiz};
    quiz1.quiz = pick_quiz(p, dice, -1);
    QuestStep guess1{StepKind::Guess};
    guess1.example = pick_example(p, dice, -1);
    QuestStep write{StepKind::Write};
    write.lesson = next_lesson(p, dice);
    QuestStep guess2{StepKind::Guess};
    guess2.example = pick_example(p, dice, guess1.example);
    QuestStep quiz2{StepKind::Quiz};
    quiz2.quiz = pick_quiz(p, dice, quiz1.quiz);
    if (write.lesson < 0) {
        write = guess2;
        write.example = pick_example(p, dice, guess2.example);
    }
    q.steps = {quiz1, guess1, write, guess2, quiz2};
    return q;
}

int choose_challenge(const Progress& p, Quest& q, const std::string& file, const std::vector<MutationToggle>& toggles) {
    if (toggles.empty()) return -1;
    int pick = -1;
    for (std::size_t i = 0; i < toggles.size() && pick < 0; ++i) {
        const std::string used = file + "#" + std::to_string(i);
        const std::string right = file + "|" + toggles[i].label + "@" + std::to_string(toggles[i].line);
        if (!q.used.count(used) && !p.right.count(right)) pick = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < toggles.size() && pick < 0; ++i)
        if (!q.used.count(file + "#" + std::to_string(i))) pick = static_cast<int>(i);
    if (pick < 0) pick = 0;
    q.used.insert(file + "#" + std::to_string(pick));
    return pick;
}

int combo_bonus(int combo) { return combo >= 2 ? (combo - 1) * 5 : 0; }

}  // namespace sparring
