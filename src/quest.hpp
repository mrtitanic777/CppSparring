#pragma once
#include "layout.hpp"

namespace sparring {

// Builds today's quest: quiz, guess, write, fix, quiz (about five minutes, with a clear finish line). Every third quest is a
// Bug Boss instead: five quick guesses, each right one knocks the bug back, right answers in a row make a combo.
Quest make_quest(const Progress& p);

constexpr int kQuestsPerBoss = 3;
constexpr int kBossHp = 3;
constexpr float kBossSeconds = 30.0f;
int quests_until_boss(const Progress& p);   // 0: the next quest is a Bug Boss

// For a Guess step whose program has been read: the challenge to ask (one not yet guessed right, and new to this quest).
int choose_challenge(const Progress& p, Quest& q, const std::string& file, const std::vector<MutationToggle>& toggles);

// The combo bonus for a right guess in a Bug Boss.
int combo_bonus(int combo);

}  // namespace sparring
