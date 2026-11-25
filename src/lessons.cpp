#include "lessons.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"

namespace sparring {

namespace {

namespace stdfs = std::filesystem;

stdfs::path exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return stdfs::path(buf).parent_path();
}

// Which course a lesson belongs to, by its number (CodingTeacher's course order).
int course_of(int n) {
    if (n <= 4) return 1;
    if (n <= 9) return 2;
    if (n <= 15) return 3;
    if (n <= 19) return 4;
    if (n <= 22) return 5;
    if (n <= 27) return 6;
    return 7;
}

std::vector<Lesson> load() {
    std::vector<Lesson> out;
    std::error_code ec;
    std::vector<stdfs::path> files;
    for (const auto& e : stdfs::directory_iterator(exe_dir() / "lessons", ec))
        if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const stdfs::path& p : files) {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        json::JParser parser(text);
        const json::JValue v = parser.parse();
        if (v.type != json::JType::Object) continue;
        Lesson l;
        const std::u8string stem = p.stem().u8string();
        l.id.assign(stem.begin(), stem.end());
        l.number = std::atoi(l.id.c_str());
        l.title = v.text("title");
        if (const json::JValue* ins = v.find("instructions")) {
            if (ins->type == json::JType::Array)
                for (const json::JValue& s : ins->arr) l.instructions.push_back(s.str);
            else l.instructions.push_back(ins->str);
        }
        l.task = v.text("task");
        l.starter = v.text("starter_code");
        l.hint = v.text("hint");
        l.expected = v.text("expected_output");
        l.course = course_of(l.number);
        if (!l.title.empty()) out.push_back(std::move(l));
    }
    return out;
}

}  // namespace

const std::vector<Course>& courses() {
    static const std::vector<Course> all = {
        {1, "Getting started"}, {2, "Boxes and math"}, {3, "Choices and loops"}, {4, "Lists and words"},
        {5, "Pointers and memory"}, {6, "Jobs and blueprints"}, {7, "The rest of C"},
    };
    return all;
}

const std::vector<Lesson>& lessons() {
    static const std::vector<Lesson> all = load();
    return all;
}

std::string normalize_output(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    lines.push_back(cur);
    for (std::string& s : lines)
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::size_t first = 0;
    while (first < lines.size() && lines[first].empty()) ++first;
    std::string out;
    for (std::size_t i = first; i < lines.size(); ++i) {
        if (i > first) out += '\n';
        out += lines[i];
    }
    return out;
}

LessonCheck check_lesson(const Lesson& l, const std::string& code) {
    LessonCheck c;
    c.run = compile_and_run_c(code);
    // with nothing to compare against, the learner must at least have written something
    const bool wrote = normalize_output(code) != normalize_output(l.starter);
    c.passed = c.run.compiled && !c.run.crashed && !c.run.timed_out &&
               (l.expected.empty() ? wrote : normalize_output(c.run.output) == normalize_output(l.expected));
    return c;
}

}  // namespace sparring
