#pragma once
// Makes practice lessons out of a programming book (a PDF) or an SDK folder. No AI is involved: the lessons are built from the
// material's own words and code samples, and every lesson is checked by really building and running its program.
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "pdf.hpp"

namespace sparring {

// ---- where imported courses are kept: %LOCALAPPDATA%\CppSparring\courses\<name>\ (course.json and one .json per lesson)
std::filesystem::path courses_root();
void use_courses_dir(const std::wstring& path);   // the self-test keeps its courses somewhere else

struct ImportedCourse {
    std::string folder;     // the folder's name, which lesson ids start with
    std::string name;       // "Tiny C++ Book"
    std::string source;     // the file or folder it was made from
    std::string language;   // "C", "C++" or "C and C++"
    std::string date;       // "2026-11-27"
    int lessons = 0;
    int programs = 0;
};
std::vector<ImportedCourse> imported_courses();
bool remove_imported_course(const std::string& folder);

// ---- reading a book into sections
struct CodeBlock {
    std::string code;                 // the lines, with their indentation
    std::vector<std::string> before;  // the paragraphs just before it
    int page = 0;
    int language = 0;                 // 0: not known, 1: C, 2: C++ (SDK files say so by their name)
    bool whole_file = false;          // an SDK sample: a complete program already
};
struct Section {
    std::string title;
    std::vector<std::string> paragraphs;
    std::vector<CodeBlock> blocks;
};
std::vector<Section> sections_from_pdf(const pdf::Document& doc);
std::vector<Section> sections_from_folder(const std::filesystem::path& folder);

// ---- trying the code samples
struct Program {
    std::string code;      // what the lesson starts with
    bool cpp = true;
    std::string input;     // typed answers it was run with ("" for none)
    std::string output;    // what it really printed
    std::size_t section = 0;
    std::size_t block = 0;
};

// Wraps a few lines of C or C++ into a whole program, or leaves a program that has main as it is. The candidates come in the
// order to try them. `prefer_c`: with nothing to tell C from C++, try C first.
struct Candidate {
    std::string code;
    bool cpp = true;
};
std::vector<Candidate> candidates_for(const CodeBlock& block, bool prefer_c);

// One line that can be taken out of a program, leaving `// ...`. `ok` is false when nothing suitable was found.
struct Gap {
    bool ok = false;
    std::string starter;   // the program with the gap
    std::string line;      // the line that was taken out (without its indentation)
};
Gap make_gap(const Program& p);

struct Change {
    bool ok = false;
    std::string starter;   // the original program
    std::string solution;  // the program with the number changed
    std::string line;      // the line before the change, trimmed
    std::string new_line;  // and after
    std::string from, to;  // the number, before and after
    std::string output;    // what the changed program prints
};
Change make_change(const Program& p);

// ---- the whole job
struct ImportOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;      // plain words, set when ok is false
    std::string folder;     // the course folder's name
    std::string name;       // the book's name
    int lessons = 0;
    int programs = 0;
};

// Called with a line to show ("Reading page 37 of 210"); return false to stop. It may be called from any thread, one at a time.
using ImportProgressFn = std::function<bool(const std::string&)>;

ImportOutcome import_pdf(const std::wstring& path, const ImportProgressFn& progress);
ImportOutcome import_folder(const std::wstring& path, const ImportProgressFn& progress);

// A development helper: `cppsparring --import IN COURSES_DIR LOG` reads a PDF or a folder, shows what it found, and makes the course.
int import_cli(const std::wstring& input, const std::wstring& courses_dir, const std::wstring& log);

}  // namespace sparring
