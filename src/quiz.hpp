#pragma once
#include <array>
#include <string>
#include <vector>

namespace sparring {

// Quick recall cards. The first ten are the "Test Yourself" questions from CodingTeacher's NOTES.md (recalling beats re-reading);
// the rest match the programs here. Answering from memory is what makes it stick.
struct QuizCard {
    const char* id;
    const char* question;
    const char* code;                     // a few lines to look at, or nullptr
    std::array<const char*, 4> answers;   // answers[0] is the right one; they are shown mixed up
    const char* why;
};

const std::vector<QuizCard>& quiz_cards();

// The order the answers of card `index` are shown in (positions into answers[]).
std::array<int, 4> answer_order(std::size_t index);

}  // namespace sparring
