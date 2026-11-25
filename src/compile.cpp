#include "compile.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace sparring {

namespace stdfs = std::filesystem;

namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string temp_dir() {
    std::error_code ec;
    stdfs::path p = stdfs::temp_directory_path(ec) / "CppSparring";
    stdfs::create_directories(p, ec);
    return p.string();
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

CompileResult build_and_run(const std::string& source_path, const std::string& include_dir, const std::string& tag, bool c_lang = false,
                            const std::string& run_dir = {}) {
    CompileResult r;
    std::error_code ec;
    const std::string exe = (stdfs::path(temp_dir()) / ("cppsparring_" + tag + ".exe")).string();
    stdfs::remove(exe, ec);

    std::string cmd = c_lang ? "clang -x c -std=c17 -D_CRT_SECURE_NO_WARNINGS" : "clang++ -std=c++17";
    cmd += " -O0 -g -fno-color-diagnostics -fno-caret-diagnostics";
    if (!include_dir.empty()) cmd += " -I" + quote(include_dir);
    cmd += " -o " + quote(exe) + " " + quote(source_path);
    ProcessResult c = run_process(cmd, 120000);
    r.compile_errors = c.output;
    if (!stdfs::exists(exe, ec)) return r;
    r.compiled = true;

    ProcessResult run = run_process(quote(exe), 5000, true, {}, run_dir);
    r.output = run.output;
    r.exit_code = run.exit_code;
    r.timed_out = run.timed_out;
    r.crashed = !run.timed_out && run.exit_code >= 0xC0000000UL;
    stdfs::remove(exe, ec);
    return r;
}

}  // namespace

ProcessResult run_process(const std::string& command_line, unsigned long timeout_ms, bool merge_stderr,
                          const std::function<void(const char*, std::size_t)>& on_chunk, const std::string& cwd) {
    ProcessResult result;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return result;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write_end;
    si.hStdError = merge_stderr ? write_end : nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = to_wide(command_line);
    const std::wstring dir = to_wide(cwd);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read_end);
        return result;
    }
    result.started = true;

    std::thread reader([&] {
        char buf[64 * 1024];
        DWORD got = 0;
        while (ReadFile(read_end, buf, sizeof buf, &got, nullptr) && got > 0) {
            if (on_chunk) on_chunk(buf, got);
            else if (result.output.size() < 256 * 1024) result.output.append(buf, got);
        }
    });

    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    reader.join();
    CloseHandle(read_end);
    return result;
}

CompileResult compile_and_run(const std::string& source_path) {
    return build_and_run(source_path, stdfs::path(source_path).parent_path().string(), "original");
}

CompileResult compile_and_run_string(const std::string& source, const std::string& original_path) {
    std::error_code ec;
    const stdfs::path src = stdfs::path(temp_dir()) / ("mutated" + stdfs::path(original_path).extension().string());
    {
        std::ofstream f(src, std::ios::binary);
        f << source;
    }
    CompileResult r = build_and_run(src.string(), stdfs::path(original_path).parent_path().string(), "mutated");
    stdfs::remove(src, ec);
    return r;
}

CompileResult compile_and_run_c(const std::string& source) {
    std::error_code ec;
    const stdfs::path dir = stdfs::path(temp_dir()) / "lesson";
    stdfs::create_directories(dir, ec);
    const stdfs::path src = dir / "lesson.c";
    {
        std::ofstream f(src, std::ios::binary);
        f << source;
    }
    return build_and_run(src.string(), {}, "lesson", true, dir.string());
}

}  // namespace sparring
