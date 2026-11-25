#pragma once
#include <string>
#include <vector>

namespace sparring {

// The programs a beginner can start from without finding a file of their own, in four levels.
struct Example {
    int level;            // 1..4
    const char* file;     // "piggy_bank.cpp"
    const char* title;    // "Piggy bank"
    const char* blurb;    // one line a ten-year-old understands
    wchar_t glyph;        // Segoe Fluent Icons
    const char* source;
};

struct Level {
    int number;
    const char* name;     // "Boxes"
    const char* teaches;  // "Boxes hold numbers and words."
};

const std::vector<Level>& levels();
const std::vector<Example>& examples();
std::vector<std::size_t> examples_in_level(int level);   // indexes into examples()

// Writes the example into the user's temp folder and returns its path (UTF-8).
std::string write_example(const Example& e);

}  // namespace sparring
