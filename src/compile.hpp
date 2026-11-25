#pragma once
#include <functional>
#include <string>

namespace sparring {

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    unsigned long exit_code = 0;
    std::string output;
};

// Runs a command line with no console window and stdin from NUL. Output goes to `on_chunk` when given, else into `output`.
ProcessResult run_process(const std::string& command_line, unsigned long timeout_ms, bool merge_stderr = true,
                          const std::function<void(const char*, std::size_t)>& on_chunk = {}, const std::string& cwd = {});

struct CompileResult {
    bool compiled = false;
    std::string compile_errors;
    std::string output;
    unsigned long exit_code = 0;
    bool crashed = false;
    bool timed_out = false;
};

CompileResult compile_and_run(const std::string& source_path);

// Compiles `source` as if it were `original_path` (its folder stays on the include path).
CompileResult compile_and_run_string(const std::string& source, const std::string& original_path);

// A lesson's program, written in C. It runs in a folder of its own, so files it writes land there.
CompileResult compile_and_run_c(const std::string& source);

}  // namespace sparring
