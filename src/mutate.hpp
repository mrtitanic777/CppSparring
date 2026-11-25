#pragma once
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// The ways to break the program, each said in plain words.
std::vector<MutationToggle> generate_mutations(const std::vector<AstNode>& nodes, const std::string& source);

// The program with one change made.
std::string apply_mutation(const std::string& source, const MutationToggle& t);

// The changed line before and after the change (tabs as spaces).
std::string line_before(const std::string& source, const MutationToggle& t);
std::string line_after(const std::string& source, const MutationToggle& t);

}  // namespace sparring
