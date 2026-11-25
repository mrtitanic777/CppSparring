#pragma once
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// The program's jobs, blueprints and boxes in sentences a beginner can read.
std::vector<LookItem> describe(const std::vector<AstNode>& nodes);

// What really happened to the changed program, compared with the original.
Outcome classify(const CompileResult& original, const CompileResult& changed);

const char* outcome_title(Outcome o);   // "It won't build"
const char* outcome_sub(Outcome o);     // "The computer can't understand it"

// Why it happened, filled in with the real names.
std::string explain_result(const MutationToggle& t, Outcome o, const CompileResult& changed);

// Where this very bug shows up in real games and apps: one line, so it feels like it matters.
std::string real_life(const MutationToggle& t);

// The first thing the compiler complained about, without the file path.
std::string plain_error(const std::string& compiler_output);

}  // namespace sparring
