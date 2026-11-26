#pragma once
#include <set>
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// Fix it: the program with one change that really breaks it, as a lesson to repair. Its task is to make the program print what
// the working one prints. It builds and runs the program and its changes, so it takes a few seconds: run it off the window's thread.
struct FixPuzzle {
    bool ok = false;
    std::string error;   // why there is none, in plain words
    Lesson lesson;
};

// `name`: the file's name ("snack_count.cpp"); `fixed`: fix_key()s already fixed (tried last); `seed`: which bug comes first.
FixPuzzle make_fix(const std::string& path, const std::string& name, const std::string& source, const std::vector<MutationToggle>& toggles,
                   const std::string& input, const std::set<std::string>& fixed, unsigned seed);

std::string fix_key(const std::string& file, const MutationToggle& t);   // "fix:snack_count.cpp|Change + into -@7"

}  // namespace sparring
