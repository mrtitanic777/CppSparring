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

// Runs a command line with no console window. It reads `input` as if typed (nothing: stdin is NUL). Output goes to `on_chunk`
// when given, else into `output`.
ProcessResult run_process(const std::string& command_line, unsigned long timeout_ms, bool merge_stderr = true,
                          const std::function<void(const char*, std::size_t)>& on_chunk = {}, const std::string& cwd = {},
                          const std::string& input = {});

// The Clang to use, quoted for a command line: on the PATH, else inside Visual Studio, else in C:\Program Files\LLVM.
std::string clang_exe(bool cpp);

// What this computer has for building programs, checked by really building and running a tiny one.
struct ToolCheck {
    bool visual_studio = false;   // Visual Studio 2022+ with the C++ workload
    bool clang = false;           // a Clang was found
    bool builds = false;          // a test program built and ran
    std::string problem;          // what the compiler said when the test failed
    std::string installer;        // the Visual Studio Installer, when it is there
};
ToolCheck check_tools();

// Whether a program asks the person running it to type something.
bool reads_input(const std::string& source);

struct CompileResult {
    bool compiled = false;
    std::string compile_errors;
    std::string output;
    unsigned long exit_code = 0;
    bool crashed = false;
    bool timed_out = false;
};

// `input`: what the program reads, as if typed (one answer per line).
CompileResult compile_and_run(const std::string& source_path, const std::string& input = {});

// Compiles `source` as if it were `original_path` (its folder stays on the include path).
CompileResult compile_and_run_string(const std::string& source, const std::string& original_path, const std::string& input = {});

// A lesson's program, in C or C++. It runs in a folder of its own, so files it writes land there.
CompileResult compile_and_run_lesson(const std::string& source, bool cpp, const std::string& input = {});

}  // namespace sparring
