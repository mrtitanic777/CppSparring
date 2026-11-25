#pragma once
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// Watching a program run one line at a time. The program gets small report calls after its lines (where its boxes are, Clang said),
// is built and run for real, and the reports become the steps (Replay, in layout.hpp).

// The program with its report calls (exposed for testing).
std::string instrument(const std::string& source, const std::vector<AstNode>& nodes);

// Builds, runs and reads back `source` (the lines must match `nodes`; a challenge's change keeps every line where it was).
Replay record_replay(const std::string& source, const std::vector<AstNode>& nodes, const std::string& original_path);

}  // namespace sparring
