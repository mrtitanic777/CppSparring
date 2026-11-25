#pragma once
#include <string>
#include <vector>

#include "compile.hpp"

namespace sparring {

// The write-it-yourself lessons, brought over from CodingTeacher's LearnC: read the idea, write a little C, and the real compiler
// checks that it prints what it should. They are .json files in the "lessons" folder beside the program, so they can be edited
// or added to without rebuilding.
struct Lesson {
    std::string id;                          // the file name without .json: "05_variables_types"
    int number = 0;                          // 5
    std::string title;
    std::vector<std::string> instructions;   // one paragraph each; "- " starts a bullet
    std::string task;
    std::string starter;
    std::string hint;
    std::string expected;                    // empty: it only has to build and run
    int course = 1;                          // 1..7
};

struct Course {
    int number;
    const char* name;
};

const std::vector<Course>& courses();
const std::vector<Lesson>& lessons();   // loaded once, in file-name order

// The result of running a lesson's code.
struct LessonCheck {
    CompileResult run;
    bool passed = false;
};
LessonCheck check_lesson(const Lesson& l, const std::string& code);

// Trailing spaces and blank lines at either end do not count (as in CodingTeacher).
std::string normalize_output(const std::string& text);

}  // namespace sparring
