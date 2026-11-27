#include "lessons.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "importer.hpp"
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

// Reads every lesson .json in a folder. `prefix` goes in front of each id (an imported course's folder, so ids stay unique).
std::vector<Lesson> load_folder(const stdfs::path& dir, const std::string& prefix, int course_number) {
    std::vector<Lesson> out;
    std::error_code ec;
    std::vector<stdfs::path> files;
    for (const auto& e : stdfs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json" && e.path().filename() != "course.json") files.push_back(e.path());
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
        const std::string file_id(stem.begin(), stem.end());
        l.id = prefix + file_id;
        l.number = std::atoi(file_id.c_str());
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
        l.input = v.text("input");
        l.practice = v.text("practice");
        l.cpp = v.text("language") == "cpp";
        if (const json::JValue* n = v.find("number")) l.number = static_cast<int>(n->num);
        if (course_number > 0) l.course = course_number;
        else if (const json::JValue* c = v.find("course")) l.course = static_cast<int>(c->num);
        else l.course = course_of(l.number);
        if (!l.title.empty()) out.push_back(std::move(l));
    }
    return out;
}

struct Store {
    std::vector<Lesson> all;
    std::vector<Course> courses;
    std::size_t builtin = 0;
};

const std::vector<Course>& builtin_courses() {
    static const std::vector<Course> all = {
        {10, "C++ 1", "C++: first steps"},     {11, "C++ 2", "C++: jobs, references and blueprints"},
        {12, "C++ 3", "C++: lists, words and games"}, {13, "C++ 4", "C++: bugs to watch for"},
        {1, "C 1", "C: getting started"},      {2, "C 2", "C: boxes and math"},
        {3, "C 3", "C: choices and loops"},    {4, "C 4", "C: lists and words"},
        {5, "C 5", "C: pointers and memory"},  {6, "C 6", "C: jobs and blueprints"},
        {7, "C 7", "C: the rest of C"},
    };
    return all;
}

Store& store() {
    static Store s = [] {
        Store st;
        st.all = load_folder(exe_dir() / "lessons", "", 0);
        st.builtin = st.all.size();
        st.courses = builtin_courses();
        return st;
    }();
    return s;
}

}  // namespace

void reload_lessons() {
    Store& s = store();
    s.all.resize(s.builtin);
    s.courses = builtin_courses();
    int n = 0;
    for (const ImportedCourse& c : imported_courses()) {
        std::vector<Lesson> mine = load_folder(courses_root() / stdfs::path(std::u8string(c.folder.begin(), c.folder.end())), c.folder + ".", kFirstImportedCourse + n);
        if (mine.empty()) continue;
        Course course;
        course.number = kFirstImportedCourse + n;
        course.tag = "Book " + std::to_string(n + 1);
        course.name = c.name;
        course.imported = true;
        course.folder = c.folder;
        s.courses.push_back(std::move(course));
        for (Lesson& l : mine) s.all.push_back(std::move(l));
        ++n;
    }
}

const std::vector<Course>& courses() {
    static const bool first = (reload_lessons(), true);
    (void)first;
    return store().courses;
}

const std::vector<Lesson>& lessons() {
    courses();   // (reads the imported courses the first time)
    return store().all;
}

std::size_t builtin_lesson_count() { return store().builtin; }

std::vector<int> lesson_order() {
    std::vector<int> out;
    for (const Course& c : courses())
        for (std::size_t i = 0; i < lessons().size(); ++i)
            if (lessons()[i].course == c.number) out.push_back(static_cast<int>(i));
    return out;
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

bool lesson_reads_input(const Lesson& l) { return reads_input(l.starter) || reads_input(l.hint); }

LessonCheck check_lesson(const Lesson& l, const std::string& code, const std::string& input) {
    LessonCheck c;
    c.run = l.path.empty() ? compile_and_run_lesson(code, l.cpp, input) : compile_and_run_string(code, l.path, input);
    // with nothing to compare against, the learner must at least have written something
    const bool wrote = normalize_output(code) != normalize_output(l.starter);
    c.passed = c.run.compiled && !c.run.crashed && !c.run.timed_out &&
               (l.expected.empty() ? wrote : normalize_output(c.run.output) == normalize_output(l.expected));
    return c;
}

}  // namespace sparring
