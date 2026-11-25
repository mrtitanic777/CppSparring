#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "window.hpp"

// cppsparring [FILE]
// cppsparring --screenshot OUT.png [--file FILE|example:N] [--page pick|look|break|badges] [--guess 0-3] [--replay STEP] [--level N] [--demo] [--dark]
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    if (!args.empty() && args[0] == L"--selftest" && args.size() >= 2) return sparring::self_test(args[1]);
    if (!args.empty() && args[0] == L"--screenshot" && args.size() >= 2) {
        sparring::ShotOptions o;
        o.out_png = args[1];
        for (std::size_t i = 2; i < args.size(); ++i) {
            const bool more = i + 1 < args.size();
            if (args[i] == L"--file" && more) o.file = args[++i];
            else if (args[i] == L"--page" && more) o.page = args[++i];
            else if (args[i] == L"--guess" && more) o.guess = std::stoi(args[++i]);
            else if (args[i] == L"--replay" && more) o.replay_step = std::stoi(args[++i]);
            else if (args[i] == L"--level" && more) o.level = std::stoi(args[++i]);
            else if (args[i] == L"--demo") o.demo = true;
            else if (args[i] == L"--dark") o.dark = true;
            else if (args[i] == L"--lesson" && more) o.lesson = std::stoi(args[++i]);
            else if (args[i] == L"--run") o.run_lesson = true;
            else if (args[i] == L"--hint") o.hint = true;
            else if (args[i] == L"--quiz" && more) o.quiz = std::stoi(args[++i]);
            else if (args[i] == L"--quest" && more) o.quest_step = std::stoi(args[++i]);
            else if (args[i] == L"--boss") o.boss = true;
            else if (args[i] == L"--chest") o.chest = true;
        }
        return sparring::screenshot(o);
    }
    return sparring::run_window(args.empty() ? std::wstring() : args[0]);
}
