#pragma once
#include <string>
#include <vector>

#include "layout.hpp"

namespace sparring {

// The declarations and references of one C++ file, from Clang's JSON AST. Only the top-level declarations written in that file are
// kept, read as the dump streams by, so a program that includes <iostream> costs a few megabytes, not the whole standard library.
std::vector<AstNode> parse_ast(const std::string& source_path, std::string* error = nullptr);

}  // namespace sparring
