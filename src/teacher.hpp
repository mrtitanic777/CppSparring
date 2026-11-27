#pragma once
#include <string>

#include "lessons.hpp"

namespace sparring {

// What Bit, the lesson buddy, says and where it points. Everything here is worked out from the code itself (no AI): the same code
// always gets the same answer.
struct TeacherSay {
    std::string text;        // what is said (and shown in the speech bubble)
    std::string code;        // a line of code shown under it, or empty
    int line = 0;            // the line pointed at (1-based), 0 for none
    int line_to = 0;         // the last line of the block lit up around it (== line for one line)
};

// One line of `code` (1-based), said in plain words.
std::string explain_line(const std::string& code, int line);
TeacherSay explain_at(const std::string& code, int line);   // the same, pointing at it (and lighting up a whole block for a loop or an if)

// The line a lesson's hint is about, and the hint said out loud.
TeacherSay hint_help(const Lesson& l, const std::string& code);
// Why the last run did not pass, pointing at the line to look at.
TeacherSay why_help(const Lesson& l, const LessonCheck& check, bool ran, const std::string& code);
// Hello (by name, when there is one), and the task.
TeacherSay intro(const Lesson& l, const std::string& name = {});

// A first name from what was typed (or said): "  my name is sam smith" -> "Sam". Empty when there is no name in it.
std::string clean_name(const std::string& typed);
// The lesson read out loud: its title, what it says and the task.
std::string read_aloud(const Lesson& l);

// The line a { on `line` closes on (or the line itself).
int block_end(const std::string& code, int line);
// The first loop or if (a line that opens a block, not main), 0 for none.
int first_block_line(const std::string& code);

// What a spoken (or typed) question asks for.
enum class Ask { None, Line, ThisLine, Hint, Why, Read, Run, Hello, Bye, Thanks, Word };
struct Heard {
    Ask ask = Ask::None;
    int line = 0;
    std::string word;   // Word: which code word ("return 0", "cout", "==")
};

// A code word explained ("what does return 0 mean?"), pointing at the first line of `code` that uses it.
TeacherSay word_help(const std::string& word, const std::string& code);
Heard understand(const std::string& said);

// Every phrase understand() knows, for the speech recognizer's word list. Numbers are said as words ("line twenty one").
std::vector<std::string> spoken_phrases();

// Text as it should be said out loud: code signs said in words, shorthand spelled out.
std::string spoken(const std::string& text);

}  // namespace sparring
