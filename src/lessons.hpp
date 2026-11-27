#pragma once
#include <string>
#include <vector>

#include "compile.hpp"

namespace sparring {

// The write-it-yourself lessons: read the idea, write a little code, and the real compiler checks that it prints what it should.
// The C ones come from CodingTeacher's LearnC; the C++ ones match the programs in Practice. They are .json files in the "lessons"
// folder beside the program, so they can be edited or added to without rebuilding.
struct Lesson {
    std::string id;                          // the file name without .json: "05_variables_types", "cpp03_changing_a_box"
    int number = 0;                          // its number inside its track
    bool cpp = false;                        // C++ (else C)
    std::string title;
    std::vector<std::string> instructions;   // one paragraph each; "- " starts a bullet
    std::string task;
    std::string starter;
    std::string hint;
    std::string expected;                    // empty: it only has to build and run (and be changed)
    std::string input;                       // what to type when it asks (the answers box starts with this)
    int course = 1;
    std::string path;                        // a Fix it puzzle: the program it came from (it is built as if it were that file)
    std::string bug_kind;                    // a Fix it puzzle: its kind of bug (bug_kinds())
    std::string practice;                    // a Practice program with the same idea in it ("pizza_party.cpp"), or empty
};

constexpr int kFirstImportedCourse = 100;   // courses made from a book or SDK are numbered from here

struct Course {
    int number = 0;
    std::string tag;    // "C++ 1", "C 3", "Book 1"
    std::string name;
    bool imported = false;   // made from a book or an SDK (Settings)
    std::string folder;      // imported: its folder under the courses folder
};

const std::vector<Course>& courses();     // in the order they are shown and taken: the C++ track first, then the imported courses
const std::vector<Lesson>& lessons();     // the built-in lessons in file-name order, then each imported course's
std::vector<int> lesson_order();          // lessons() indexes in course order
// Reads the imported courses again (after one was made or removed). The built-in lessons keep their places in lessons(); the imported
// ones come after them, so an index of an imported lesson means something else afterwards.
void reload_lessons();
std::size_t builtin_lesson_count();

// The result of running a lesson's code.
struct LessonCheck {
    CompileResult run;
    bool passed = false;
};
LessonCheck check_lesson(const Lesson& l, const std::string& code, const std::string& input = {});

// Whether a lesson's program asks for typed input (its code or its hint reads it).
bool lesson_reads_input(const Lesson& l);

// Trailing spaces and blank lines at either end do not count (as in CodingTeacher).
std::string normalize_output(const std::string& text);

}  // namespace sparring
