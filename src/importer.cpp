#include "importer.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "compile.hpp"
#include "json.hpp"
#include "lessons.hpp"

namespace sparring {

namespace fs = std::filesystem;

namespace {

// =====================================================================================================================
// Small text helpers
// =====================================================================================================================

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string collapse_spaces(const std::string& s) {
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c) || c == 0xA0) {
            space = !out.empty();
        } else {
            if (space) out += ' ';
            space = false;
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool ends_with_char(const std::string& s, char c) { return !s.empty() && s.back() == c; }

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string path_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path path_from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// A book's typesetting turned back into what a keyboard types.
std::string plain_code_chars(std::string s) {
    const std::pair<const char*, const char*> swaps[] = {
        {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""}, {"\xE2\x80\x98", "'"},  {"\xE2\x80\x99", "'"},  {"\xE2\x80\x93", "-"},  {"\xE2\x88\x92", "-"},
        {"\xE2\x80\xA6", "..."}, {"\xC2\xA0", " "},     {"\xE2\x80\x8B", ""},    {"\xEF\xBB\xBF", ""},    {"\xEF\xAC\x81", "fi"}, {"\xEF\xAC\x82", "fl"},
        {"\xC2\xAD", ""},       {"\xE2\x80\x90", "-"},  {"\xE2\x80\x91", "-"},   {"\xE2\x80\xB2", "'"},   {"\xE2\x80\x82", " "},  {"\xE2\x80\x83", " "},
    };
    for (const auto& [from, to] : swaps) replace_all(s, from, to);
    return s;
}

// Source with the insides of strings, characters and comments blanked out (same length, same lines), so a search sees only code.
std::string blank_literals(const std::string& s) {
    std::string out = s;
    enum { Code, Str, Chr, Line, Block } st = Code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const char n = i + 1 < out.size() ? out[i + 1] : '\0';
        switch (st) {
        case Code:
            if (c == '"') st = Str;
            else if (c == '\'') st = Chr;
            else if (c == '/' && n == '/') { st = Line; out[i] = out[i + 1] = ' '; ++i; }
            else if (c == '/' && n == '*') { st = Block; out[i] = out[i + 1] = ' '; ++i; }
            break;
        case Str:
        case Chr:
            if (c == '\\' && i + 1 < out.size()) {
                out[i] = ' ';
                if (out[i + 1] != '\n') out[i + 1] = ' ';
                ++i;
            } else if ((st == Str && c == '"') || (st == Chr && c == '\'') || c == '\n') {
                if (c == '\n') st = Code;
                else st = Code;
            } else {
                out[i] = ' ';
            }
            break;
        case Line:
            if (c == '\n') st = Code;
            else out[i] = ' ';
            break;
        case Block:
            if (c == '*' && n == '/') { out[i] = out[i + 1] = ' '; ++i; st = Code; }
            else if (c != '\n') out[i] = ' ';
            break;
        }
    }
    return out;
}

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Whether `word` appears as a whole word.
bool has_word(const std::string& s, const char* word) {
    const std::size_t len = std::strlen(word);
    for (std::size_t at = 0; (at = s.find(word, at)) != std::string::npos; at += len) {
        const bool left = at == 0 || !is_ident(s[at - 1]);
        const bool right = at + len >= s.size() || !is_ident(s[at + len]);
        if (left && right) return true;
    }
    return false;
}

// "main (" as a word followed by an opening bracket.
bool has_main(const std::string& blanked) {
    for (std::size_t at = 0; (at = blanked.find("main", at)) != std::string::npos; at += 4) {
        if (at > 0 && is_ident(blanked[at - 1])) continue;
        std::size_t p = at + 4;
        while (p < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[p]))) ++p;
        if (p < blanked.size() && blanked[p] == '(') return true;
    }
    return false;
}

bool has_cpp_markers(const std::string& blanked, const std::string& raw) {
    if (blanked.find("::") != std::string::npos) return true;
    for (const char* w : {"cout", "cin", "cerr", "endl", "class", "template", "namespace", "nullptr", "vector", "string", "auto", "new", "delete", "public", "private",
                          "iostream", "fstream", "sstream", "std", "bool", "true", "false", "constexpr", "typename", "operator", "virtual", "try", "catch"}) {
        if (std::strcmp(w, "bool") == 0 || std::strcmp(w, "true") == 0 || std::strcmp(w, "false") == 0) continue;   // C has them too
        if (has_word(blanked, w)) return true;
    }
    for (const char* inc : {"<string>", "<vector>", "<iostream>", "<map>", "<set>", "<algorithm>", "<memory>", "<cmath>", "<cstdio>", "<cstdlib>", "<cstring>"})
        if (raw.find(inc) != std::string::npos) return true;
    return false;
}

bool has_c_markers(const std::string& blanked, const std::string& raw) {
    for (const char* inc : {"<stdio.h>", "<stdlib.h>", "<string.h>", "<math.h>", "<ctype.h>", "<stdbool.h>"})
        if (raw.find(inc) != std::string::npos) return true;
    for (const char* w : {"printf", "scanf", "malloc", "free", "fgets", "puts", "strlen", "strcpy", "typedef", "struct"})
        if (has_word(blanked, w)) return true;
    return false;
}

// Code that could do harm, or that never prints the same thing twice. It is never run.
bool unsafe_or_unsteady(const std::string& blanked, const std::string& raw) {
    for (const char* w : {"system", "popen", "fork", "exec", "execl", "execv", "remove", "rename", "unlink", "rmdir", "rand", "srand", "time", "clock", "getenv",
                          "sleep", "usleep", "Sleep", "signal", "raise", "abort", "chdir", "mkdir", "fopen", "freopen", "tmpfile", "ofstream", "fstream", "ifstream",
                          "random_device", "chrono", "thread", "pthread_create", "CreateProcess", "ShellExecute", "WinExec", "LoadLibrary", "socket", "connect",
                          "bind", "listen", "accept", "gets", "getpid", "mt19937", "uniform_int_distribution", "ctime", "localtime", "gmtime", "difftime"})
        if (has_word(blanked, w)) return true;
    for (const char* inc : {"<windows.h>", "<unistd.h>", "<conio.h>", "<sys/", "<thread>", "<future>", "<filesystem>", "<chrono>", "<random>", "<winsock", "<pthread.h>"})
        if (raw.find(inc) != std::string::npos) return true;
    return false;
}

// =====================================================================================================================
// Where the courses are kept
// =====================================================================================================================

std::wstring& courses_override() {
    static std::wstring p;
    return p;
}

std::string today_string() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return buf;
}

std::string read_text_file(const fs::path& p, std::size_t limit = 4u * 1024 * 1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > limit) s.resize(limit);
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) s.erase(0, 3);
    return s;
}

std::string safe_name(const std::string& name_in) {
    std::string name = name_in;
    replace_all(name, "++", "pp");
    std::string out;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 128) {
            out += static_cast<char>(std::tolower(c));
            dash = false;
        } else if (!out.empty() && !dash) {
            out += '_';
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 28) out.resize(28);
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "book" : out;
}

// =====================================================================================================================
// A book becomes sections, paragraphs and code blocks
// =====================================================================================================================

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool ends_with_digit(const std::string& t) { return !t.empty() && is_digit(t.back()); }

// "Chapter 3", "Part II", "Lesson 12", "Appendix A"
bool chapter_like(const std::string& t) {
    for (const char* w : {"Chapter ", "CHAPTER ", "Part ", "PART ", "Lesson ", "LESSON ", "Appendix ", "APPENDIX ", "Unit ", "Module ", "Section "}) {
        if (!starts_with(t, w)) continue;
        const char c = t.size() > std::strlen(w) ? t[std::strlen(w)] : '\0';
        if (is_digit(c) || c == 'I' || c == 'V' || c == 'X' || (std::isupper(static_cast<unsigned char>(c)) && (starts_with(t, "Appendix") || starts_with(t, "APPENDIX"))))
            return true;
    }
    return false;
}

// "1.2 Title", "3 Title", "4. Title"
bool numbered_like(const std::string& t, std::size_t* rest_at = nullptr) {
    std::size_t i = 0;
    if (i >= t.size() || !is_digit(t[i])) return false;
    while (i < t.size() && (is_digit(t[i]) || (t[i] == '.' && i + 1 < t.size() && is_digit(t[i + 1])))) ++i;
    if (i > 12) return false;
    if (i < t.size() && t[i] == '.') ++i;
    if (i >= t.size() || t[i] != ' ') return false;
    while (i < t.size() && t[i] == ' ') ++i;
    if (i >= t.size() || !std::isupper(static_cast<unsigned char>(t[i]))) return false;
    if (rest_at) *rest_at = i;
    return true;
}

bool looks_like_contents_line(const std::string& t) {
    if (t.find("....") != std::string::npos || t.find(". . .") != std::string::npos) return true;
    return t.size() > 20 && ends_with_digit(t) && t.find("  ") != std::string::npos;
}

// A short name for a lesson's title: no "Chapter 3" and no "2.1".
std::string short_title(const std::string& title, std::size_t max = 22) {
    std::string t = collapse_spaces(title);
    std::size_t at = 0;
    if (chapter_like(t)) {
        std::size_t i = t.find(' ');
        while (i != std::string::npos && i < t.size() && t[i] == ' ') ++i;
        // the number or numeral after the word
        std::size_t j = i;
        while (j < t.size() && t[j] != ' ' && t[j] != ':' && t[j] != '.' && t[j] != '-') ++j;
        while (j < t.size() && (t[j] == ' ' || t[j] == ':' || t[j] == '.' || t[j] == '-')) ++j;
        if (j < t.size()) t = t.substr(j);
    } else if (numbered_like(t, &at)) {
        t = t.substr(at);
    }
    t = trim(t);
    if (t.empty()) t = collapse_spaces(title);
    if (t.size() > max) {
        std::size_t cut = max - 1;
        const std::size_t sp = t.rfind(' ', cut);
        if (sp != std::string::npos && sp > max / 2) cut = sp;
        t = trim(t.substr(0, cut)) + "\xE2\x80\xA6";
    }
    return t;
}

bool is_bullet_start(const std::string& t, std::size_t* skip) {
    static const char* marks[] = {"\xE2\x80\xA2 ", "\xC2\xB7 ", "\xE2\x96\xAA ", "\xE2\x97\x8F ", "\xE2\x80\x93 ", "- ", "* "};
    for (const char* m : marks)
        if (starts_with(t, m)) {
            *skip = std::strlen(m);
            return true;
        }
    return false;
}

bool numbered_item_start(const std::string& t) {
    std::size_t i = 0;
    while (i < t.size() && i < 3 && is_digit(t[i])) ++i;
    return i > 0 && i < t.size() - 1 && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ';
}

float body_font_size(const pdf::Document& doc) {
    std::map<int, double> weight;
    for (const pdf::Page& p : doc.pages)
        for (const pdf::Line& l : p.lines)
            if (!l.mono && l.text.size() > 25) weight[static_cast<int>(std::lround(l.size * 2))] += static_cast<double>(l.text.size());
    int best = 0;
    double most = 0;
    for (const auto& [k, w] : weight)
        if (w > most) {
            most = w;
            best = k;
        }
    return best > 0 ? static_cast<float>(best) / 2.0f : 11.0f;
}

std::string clean_code(const std::vector<std::string>& lines) {
    std::vector<std::string> v = lines;
    // line numbers down the left side
    int numbered = 0, total = 0;
    for (const std::string& l : v) {
        const std::string t = trim(l);
        if (t.empty()) continue;
        ++total;
        std::size_t i = 0;
        while (i < t.size() && is_digit(t[i])) ++i;
        if (i > 0 && i <= 4 && i < t.size() && (t[i] == ' ' || ((t[i] == ':' || t[i] == '.' || t[i] == ')') && i + 1 < t.size() && t[i + 1] == ' '))) ++numbered;
    }
    if (total >= 2 && numbered * 10 >= total * 8) {
        for (std::string& l : v) {
            std::size_t ind = 0;
            while (ind < l.size() && l[ind] == ' ') ++ind;
            std::size_t i = ind;
            while (i < l.size() && is_digit(l[i])) ++i;
            if (i > ind && i - ind <= 4) {
                if (i < l.size() && (l[i] == ':' || l[i] == '.' || l[i] == ')')) ++i;
                while (i < l.size() && l[i] == ' ') ++i;
                l = l.substr(i);
            }
        }
    }
    while (!v.empty() && trim(v.front()).empty()) v.erase(v.begin());
    while (!v.empty() && trim(v.back()).empty()) v.pop_back();
    // the same left edge
    std::size_t common = std::string::npos;
    for (const std::string& l : v) {
        if (trim(l).empty()) continue;
        std::size_t ind = 0;
        while (ind < l.size() && l[ind] == ' ') ++ind;
        common = std::min(common, ind);
    }
    if (common == std::string::npos) common = 0;
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += '\n';
        out += trim(v[i]).empty() ? std::string() : v[i].substr(common);
    }
    return plain_code_chars(out);
}

bool plausible_code(const std::string& code) {
    if (code.size() < 6 || code.size() > 6000) return false;
    const std::string blanked = blank_literals(code);
    if (blanked.find(';') == std::string::npos && code.find("#include") == std::string::npos) return false;
    // lines of ordinary words with no code in them (a program's printed output, a shell session)
    return true;
}

}  // namespace

std::vector<Section> sections_from_pdf(const pdf::Document& doc) {
    std::vector<Section> sections;
    if (!doc.ok) return sections;
    const float body = body_font_size(doc);

    // running heads and page numbers: lines in the margins that come back on page after page (the same words, or the same words
    // with another number, in small plain letters), or are only a number. A chapter heading is bigger or bold, so it stays.
    std::map<std::string, int> same_count, number_count;
    auto in_margin = [](const pdf::Page& p, const pdf::Line& l) { return l.y > p.height * 0.915f || l.y < p.height * 0.085f; };
    auto margin_key = [](const pdf::Line& l) {
        std::string k;
        for (unsigned char c : l.text)
            if (!std::isdigit(c) && c != ' ') k += static_cast<char>(std::tolower(c));
        return k;
    };
    for (const pdf::Page& p : doc.pages)
        for (const pdf::Line& l : p.lines)
            if (in_margin(p, l)) {
                ++same_count[collapse_spaces(l.text)];
                ++number_count[margin_key(l)];
            }
    auto is_running_head = [&](const pdf::Line& l) {
        const std::string key = margin_key(l);
        if (key.empty()) return true;   // only a number
        if (same_count[collapse_spaces(l.text)] >= 3 && l.text.size() <= 120) return true;
        return number_count[key] >= 3 && l.size <= body * 1.05f && !l.bold && l.text.size() <= 120;
    };

    Section cur;
    cur.title = doc.title.empty() ? "Introduction" : doc.title;
    std::vector<std::string> since;   // paragraphs since the last code block
    std::string para;
    float para_x = 0, para_size = 0;
    std::vector<std::string> code_lines;
    std::vector<float> code_ys, code_sizes;
    int code_page = 0;
    float code_min_x = 1e9f, code_cw = 0, code_shift = 0;
    int code_last_page = 0;
    std::vector<float> code_xs;
    bool last_was_heading = false;
    int heading_page = -1;
    float heading_y = 0, heading_size = 0;
    int page_no = 0;

    auto flush_para = [&] {
        const std::string p = trim(para);
        para.clear();
        if (p.size() < 3) return;
        cur.paragraphs.push_back(p);
        since.push_back(p);
        if (since.size() > 8) since.erase(since.begin());
    };
    auto flush_code = [&] {
        if (code_lines.empty()) return;
        // blank lines where the lines are further apart than one line
        float pitch = 1e9f;
        for (std::size_t i = 1; i < code_ys.size(); ++i)
            if (code_ys[i - 1] > code_ys[i]) pitch = std::min(pitch, code_ys[i - 1] - code_ys[i]);
        std::vector<std::string> lines;
        const float cw = code_cw > 0 ? code_cw : 0.6f * (code_sizes.empty() ? 10.0f : code_sizes.front());
        for (std::size_t i = 0; i < code_lines.size(); ++i) {
            if (i > 0 && code_ys[i - 1] > code_ys[i] && pitch < 1e8f) {
                const int blanks = static_cast<int>(std::lround((code_ys[i - 1] - code_ys[i]) / pitch)) - 1;
                for (int b = 0; b < std::min(blanks, 2); ++b) lines.push_back("");
            }
            const int indent = std::clamp(static_cast<int>(std::lround((code_xs[i] - code_min_x) / cw)), 0, 40);
            lines.push_back(std::string(static_cast<std::size_t>(indent), ' ') + code_lines[i]);
        }
        CodeBlock b;
        b.code = clean_code(lines);
        b.before = since;
        b.page = code_page;
        since.clear();
        code_lines.clear();
        code_ys.clear();
        code_sizes.clear();
        code_xs.clear();
        code_min_x = 1e9f;
        code_cw = 0;
        if (!b.code.empty()) cur.blocks.push_back(std::move(b));
    };
    auto finish_section = [&] {
        flush_para();
        flush_code();
        if (!cur.paragraphs.empty() || !cur.blocks.empty()) sections.push_back(std::move(cur));
        cur = Section();
        since.clear();
    };

    const pdf::Line* prev = nullptr;
    int prev_page = -1;
    float page_right = 0;
    for (const pdf::Page& pg : doc.pages) {
        ++page_no;
        page_right = 0;
        for (const pdf::Line& l : pg.lines)
            if (!l.mono) page_right = std::max(page_right, l.x_end);
        bool first = true;
        const pdf::Line* last_on_page = nullptr;
        for (const pdf::Line& l : pg.lines) {
            if (in_margin(pg, l) && is_running_head(l)) continue;
            const float gap = (!first && last_on_page) ? last_on_page->y - l.y : 0.0f;
            const bool top = first;
            first = false;
            last_on_page = &l;
            const std::string t = collapse_spaces(l.text);
            if (t.empty()) continue;

            if (l.mono) {
                flush_para();
                last_was_heading = false;
                if (code_lines.empty()) {
                    code_page = page_no;
                    code_shift = 0;
                } else if (code_last_page != page_no) {   // the code carries on over the page: as if the next line came right after
                    code_shift = code_ys.back() - 1.4f * l.size - l.y;
                }
                code_last_page = page_no;
                code_lines.push_back(t);
                code_ys.push_back(l.y + code_shift);
                code_sizes.push_back(l.size);
                code_xs.push_back(l.x);
                code_min_x = std::min(code_min_x, l.x);
                if (code_cw == 0 && l.char_w > 0) code_cw = l.char_w;
                prev = &l;
                prev_page = page_no;
                continue;
            }
            flush_code();

            // is it a heading?
            bool heading = false;
            const bool big = l.size >= body * 1.18f;
            if (t.size() <= 110 && !ends_with_char(t, ',') && !ends_with_char(t, ';') && !looks_like_contents_line(t)) {
                if (big && !(ends_with_char(t, '.') && t.size() > 60)) heading = true;
                else if (chapter_like(t) && t.size() <= 70 && !ends_with_digit(t) && (l.bold || l.size >= body * 1.0f)) heading = true;
                else if (numbered_like(t) && t.size() <= 90 && (l.bold || l.size >= body * 1.08f) && !ends_with_char(t, '.')) heading = true;
                else if (l.bold && t.size() <= 60 && !ends_with_char(t, ':') && !ends_with_char(t, '.') && (top || gap >= 1.5f * l.size)) {
                    int words = 0;
                    for (char c : t) words += c == ' ';
                    heading = words < 9;
                }
            }
            if (heading) {
                flush_para();
                const bool same_look = std::fabs(l.size - heading_size) < 0.12f * heading_size || (chapter_like(cur.title) && cur.title.size() <= 14);
                const bool joins = last_was_heading && same_look && cur.paragraphs.empty() && cur.blocks.empty() && heading_page == page_no && !top &&
                                   (heading_y - l.y) < 2.4f * std::max(l.size, heading_size);
                if (joins) {
                    cur.title += " " + t;
                } else {
                    finish_section();
                    cur.title = t;
                }
                last_was_heading = true;
                heading_page = page_no;
                heading_y = l.y;
                heading_size = l.size;
                prev = &l;
                prev_page = page_no;
                continue;
            }
            last_was_heading = false;

            // a paragraph
            std::size_t skip = 0;
            const bool bullet = is_bullet_start(t, &skip);
            bool new_para = para.empty() || bullet || numbered_item_start(t);
            if (!new_para) {
                if (prev_page == page_no && prev && !prev->mono) {
                    const float line_gap = prev->y - l.y;
                    if (line_gap > 1.6f * std::max(l.size, 1.0f)) new_para = true;
                    else if (l.x > para_x + 1.1f * para_size && prev->x_end < page_right * 0.97f) new_para = true;   // an indented first line
                    else if (prev->x_end < page_right * 0.78f && (ends_with_char(para, '.') || ends_with_char(para, '!') || ends_with_char(para, '?') || ends_with_char(para, ':')))
                        new_para = true;
                } else if (prev_page != page_no) {
                    // the paragraph carries on over the page, unless it ended
                    const bool ended = ends_with_char(para, '.') || ends_with_char(para, '!') || ends_with_char(para, '?') || ends_with_char(para, ':');
                    if (ended && std::isupper(static_cast<unsigned char>(t[0]))) new_para = true;
                } else {
                    new_para = true;
                }
            }
            if (new_para) {
                flush_para();
                para_x = l.x;
                para_size = l.size;
                para = bullet ? "- " + t.substr(skip) : t;
            } else if (para.size() >= 2 && para.back() == '-' && std::isalpha(static_cast<unsigned char>(para[para.size() - 2])) && std::islower(static_cast<unsigned char>(t[0]))) {
                para.pop_back();
                para += t;
            } else {
                para += ' ';
                para += t;
            }
            prev = &l;
            prev_page = page_no;
        }
    }
    finish_section();
    return sections;
}

namespace {

// =====================================================================================================================
// An SDK folder becomes sections too: one per subfolder
// =====================================================================================================================

std::string strip_tags(const std::string& html) {
    std::string out;
    std::string lower = html;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            // script and style go away with what is inside them
            for (const char* tag : {"<script", "<style"}) {
                if (lower.compare(i, std::strlen(tag), tag) == 0) {
                    const std::string close = std::string("</") + (tag + 1);
                    const std::size_t end = lower.find(close, i);
                    i = end == std::string::npos ? html.size() : lower.find('>', end);
                    if (i == std::string::npos) i = html.size();
                    else ++i;
                    out += '\n';
                    goto next;
                }
            }
            {
                const std::size_t end = html.find('>', i);
                const bool block = lower.compare(i, 2, "<p") == 0 || lower.compare(i, 3, "<br") == 0 || lower.compare(i, 3, "<li") == 0 || lower.compare(i, 2, "<h") == 0 ||
                                   lower.compare(i, 4, "</p>") == 0 || lower.compare(i, 5, "</div") == 0 || lower.compare(i, 4, "<div") == 0 || lower.compare(i, 3, "<tr") == 0;
                if (block) out += "\n\n";
                i = end == std::string::npos ? html.size() : end + 1;
            }
        } else {
            out += html[i++];
        }
    next:;
    }
    replace_all(out, "&nbsp;", " ");
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#39;", "'");
    replace_all(out, "&amp;", "&");
    return out;
}

// Paragraphs out of a block of text: blank lines separate them, a line starting "- " or "* " or "#" is its own.
std::vector<std::string> paragraphs_of(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        const std::string p = collapse_spaces(cur);
        cur.clear();
        if (!p.empty()) out.push_back(p);
    };
    for (const std::string& raw : split_lines(text)) {
        std::string l = trim(raw);
        if (l.empty()) {
            flush();
            continue;
        }
        if (l[0] == '#') {   // a markdown heading: it ends the paragraph and is left out
            flush();
            continue;
        }
        if (l.size() > 2 && (starts_with(l, "- ") || starts_with(l, "* "))) {
            flush();
            cur = "- " + l.substr(2);
            flush();
            continue;
        }
        if (l.find("```") != std::string::npos) {
            flush();
            continue;
        }
        cur += (cur.empty() ? "" : " ") + l;
    }
    flush();
    return out;
}

// The comment at the very top of a source file (not a licence).
std::vector<std::string> header_comment(const std::string& code) {
    std::vector<std::string> lines = split_lines(code);
    std::string text;
    std::size_t i = 0;
    while (i < lines.size() && trim(lines[i]).empty()) ++i;
    if (i < lines.size() && starts_with(trim(lines[i]), "/*")) {
        for (; i < lines.size(); ++i) {
            std::string l = trim(lines[i]);
            const bool end = l.find("*/") != std::string::npos;
            if (starts_with(l, "/*")) l = l.substr(2);
            if (end) l = l.substr(0, l.find("*/"));
            while (!l.empty() && (l[0] == '*' || l[0] == ' ')) l.erase(0, 1);
            text += l + "\n";
            if (end) break;
        }
    } else {
        for (; i < lines.size(); ++i) {
            std::string l = trim(lines[i]);
            if (!starts_with(l, "//")) break;
            l = l.substr(2);
            while (!l.empty() && (l[0] == '/' || l[0] == '!' || l[0] == '-' || l[0] == '=' || l[0] == '*')) l.erase(0, 1);
            text += trim(l) + "\n";
        }
    }
    std::string lower = text;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("copyright") != std::string::npos || lower.find("license") != std::string::npos || lower.find("licence") != std::string::npos ||
        lower.find("permission") != std::string::npos || lower.find("warranty") != std::string::npos)
        return {};
    return paragraphs_of(text);
}

std::string prettify_folder(const std::string& name) {
    std::string s = name;
    for (char& c : s)
        if (c == '_' || c == '-') c = ' ';
    s = collapse_spaces(s);
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    // short words that are always capitals
    std::string out, word;
    auto flush = [&] {
        std::string lower = word;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "sdk" || lower == "api" || lower == "ui" || lower == "gui" || lower == "os" || lower == "io" || lower == "cpu" || lower == "gpu" || lower == "ide")
            for (char& c : word) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out += word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') {
            flush();
            out += ' ';
        } else {
            word += c;
        }
    }
    flush();
    return out;
}

}  // namespace

std::vector<Section> sections_from_folder(const fs::path& folder) {
    std::vector<Section> out;
    std::error_code ec;
    std::map<fs::path, std::vector<fs::path>> sources, docs;   // by the folder they are in
    int seen = 0;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (++seen > 20000) break;
        const fs::path& p = it->path();
        const std::string name = path_utf8(p.filename());
        if (it->is_directory(ec)) {
            if (name.empty() || name[0] == '.' || name == "node_modules" || name == "build" || name == "bin" || name == "obj" || name == "out" || name == "target" ||
                name == "third_party" || name == "vendor" || name == "Debug" || name == "Release")
                it.disable_recursion_pending();
            if (it.depth() >= 8) it.disable_recursion_pending();
            continue;
        }
        std::string ext = path_utf8(p.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
            if (it->file_size(ec) <= 48 * 1024) sources[p.parent_path()].push_back(p);
        } else if (ext == ".md" || ext == ".txt" || ext == ".html" || ext == ".htm" || ext == ".markdown") {
            if (it->file_size(ec) <= 512 * 1024) docs[p.parent_path()].push_back(p);
        }
    }
    const std::string root_name = path_utf8(folder.filename().empty() ? folder.parent_path().filename() : folder.filename());
    // text for a folder: its documents (README first), then the root's README for everything
    auto doc_paragraphs = [&](const fs::path& dir) {
        std::vector<fs::path> files = docs.count(dir) ? docs[dir] : std::vector<fs::path>();
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            auto readme = [](const fs::path& p) {
                std::string n = path_utf8(p.filename());
                for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return n.rfind("readme", 0) == 0 ? 0 : 1;
            };
            return std::make_pair(readme(a), a) < std::make_pair(readme(b), b);
        });
        std::vector<std::string> paras;
        for (const fs::path& f : files) {
            std::string text = read_text_file(f, 128 * 1024);
            std::string ext = path_utf8(f.extension());
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".html" || ext == ".htm") text = strip_tags(text);
            for (std::string& p : paragraphs_of(text))
                if (p.size() >= 20) paras.push_back(std::move(p));
            if (paras.size() >= 12) break;
        }
        return paras;
    };
    const std::vector<std::string> root_docs = doc_paragraphs(folder);

    std::vector<fs::path> dirs;
    for (const auto& [dir, files] : sources) dirs.push_back(dir);
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs) {
        Section s;
        const fs::path rel = fs::relative(dir, folder, ec);
        const std::string rel_name = path_utf8(rel.filename());
        s.title = (rel.empty() || rel == fs::path(".") || rel_name.empty() || rel_name == ".") ? prettify_folder(root_name) : prettify_folder(rel_name);
        s.paragraphs = doc_paragraphs(dir);
        if (s.paragraphs.empty() && dir != folder) s.paragraphs = root_docs;
        std::vector<fs::path> files = sources[dir];
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            std::string code = read_text_file(f, 64 * 1024);
            if (code.empty()) continue;
            code = plain_code_chars(code);
            replace_all(code, "\r\n", "\n");
            if (!has_main(blank_literals(code))) continue;
            std::string ext = path_utf8(f.extension());
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            CodeBlock b;
            b.code = code;
            b.language = ext == ".c" ? 1 : 2;
            b.whole_file = true;
            b.before = header_comment(code);
            if (b.before.empty()) b.before.push_back("This sample is " + path_utf8(f.filename()) + ", from " + s.title + ".");
            s.blocks.push_back(std::move(b));
        }
        if (!s.blocks.empty()) out.push_back(std::move(s));
    }
    return out;
}

// =====================================================================================================================
// Making programs out of code blocks
// =====================================================================================================================

namespace {

bool is_type_word(const std::string& w) {
    static const std::set<std::string> types = {"int", "void", "double", "float", "char", "long", "short", "unsigned", "signed", "bool", "auto", "const", "static", "size_t",
                                                "struct", "class", "enum", "union", "inline", "extern", "constexpr", "std::string", "string", "uint8_t", "uint16_t",
                                                "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "FILE", "template"};
    return types.count(w) > 0;
}

std::string first_word(const std::string& l) {
    std::size_t i = 0;
    while (i < l.size() && (is_ident(l[i]) || l[i] == ':')) ++i;
    return l.substr(0, i);
}

// Does this line (no indentation) start something that belongs outside main: a job, a blueprint, a prototype?
// `how` gets "block" (carry on to the closing brace), "line" (just this line).
bool starts_declaration(const std::vector<std::string>& lines, std::size_t i, std::string* how) {
    const std::string l = lines[i];
    if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '#' || l[0] == '}') return false;
    const std::string w = first_word(l);
    if (w == "struct" || w == "class" || w == "enum" || w == "union" || w == "namespace" || w == "template" || w == "typedef") {
        // "struct point p;" is a box, "struct point {" is a blueprint
        const std::string rest = l;
        if (rest.find('{') != std::string::npos || (i + 1 < lines.size() && trim(lines[i + 1]) == "{") || w == "template") {
            *how = "block";
            return true;
        }
        if (w == "typedef" && rest.find(';') != std::string::npos) {
            *how = "line";
            return true;
        }
        return false;
    }
    if (w == "using" && l.find("namespace") != std::string::npos) {
        *how = "line";
        return true;
    }
    if (!is_type_word(w)) return false;
    // type name ( ... : a job (or a prototype)
    const std::size_t paren = l.find('(');
    const std::size_t eq = l.find('=');
    if (paren == std::string::npos || (eq != std::string::npos && eq < paren)) return false;
    // something before the bracket besides the type: a name
    std::size_t k = paren;
    while (k > 0 && std::isspace(static_cast<unsigned char>(l[k - 1]))) --k;
    std::size_t name_end = k;
    while (k > 0 && (is_ident(l[k - 1]) || l[k - 1] == ':')) --k;
    if (name_end == k || k == 0) return false;
    if (l.find('{') != std::string::npos || (i + 1 < lines.size() && trim(lines[i + 1]) == "{") || (trim(l).back() == ')' && i + 1 < lines.size() && trim(lines[i + 1]) == "{")) {
        *how = "block";
        return true;
    }
    if (trim(l).back() == ';') {
        *how = "line";
        return true;
    }
    if (trim(l).back() == ')' && i + 1 < lines.size() && !trim(lines[i + 1]).empty() && trim(lines[i + 1])[0] == '{') {
        *how = "block";
        return true;
    }
    return false;
}

std::string indent_block(const std::string& body, const char* pad) {
    std::vector<std::string> lines = split_lines(body);
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    std::string out;
    for (const std::string& l : lines) {
        if (!trim(l).empty()) out += pad + l;
        out += '\n';
    }
    return out;
}

// Only the headers the code uses.
std::string includes_for(const std::string& blanked, bool cpp) {
    struct Need {
        const char* header_cpp;
        const char* header_c;
        std::vector<const char*> words;
    };
    const Need needs[] = {
        {"iostream", nullptr, {"cout", "cin", "cerr", "endl", "getline", "clog"}},
        {"string", nullptr, {"string", "to_string", "stoi", "stod", "getline"}},
        {"vector", nullptr, {"vector"}},
        {"map", nullptr, {"map"}},
        {"algorithm", nullptr, {"sort", "reverse", "find", "min", "max", "swap", "count", "fill", "accumulate"}},
        {"cmath", "math.h", {"sqrt", "pow", "sin", "cos", "tan", "floor", "ceil", "fabs", "abs", "round", "log", "exp", "fmod", "M_PI"}},
        {"cstdio", "stdio.h", {"printf", "scanf", "puts", "putchar", "getchar", "fgets", "sprintf", "snprintf", "fprintf", "stdin", "stdout", "FILE", "sscanf"}},
        {"cstdlib", "stdlib.h", {"malloc", "free", "calloc", "realloc", "atoi", "atof", "exit", "abs", "qsort", "strtol", "EXIT_SUCCESS", "EXIT_FAILURE"}},
        {"cstring", "string.h", {"strlen", "strcpy", "strcat", "strcmp", "memcpy", "memset", "strncpy", "strchr", "strstr", "strncmp"}},
        {nullptr, "ctype.h", {"isalpha", "isdigit", "toupper", "tolower", "isspace", "isupper", "islower", "isalnum"}},
        {nullptr, "stdbool.h", {"bool", "true", "false"}},
    };
    std::string out;
    for (const Need& n : needs) {
        const char* header = cpp ? n.header_cpp : n.header_c;
        if (!header) continue;
        bool used = false;
        for (const char* w : n.words) used = used || has_word(blanked, w);
        if (used) out += std::string("#include <") + header + ">\n";
    }
    if (out.empty()) out = cpp ? "#include <iostream>\n" : "#include <stdio.h>\n";
    return out;
}

std::string wrap_statements(const std::string& code, bool cpp) {
    const std::vector<std::string> lines = split_lines(code);
    std::string pre, outside, inside;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (!l.empty() && l[0] == '#') {
            pre += l + "\n";
            continue;
        }
        std::string how;
        if (starts_declaration(lines, i, &how)) {
            if (how == "line") {
                outside += l + "\n";
                continue;
            }
            // up to the brace that closes it (and for a blueprint, the semicolon)
            int depth = 0;
            bool opened = false;
            std::size_t j = i;
            for (; j < lines.size(); ++j) {
                const std::string b = blank_literals(lines[j]);
                for (char c : b) {
                    if (c == '{') {
                        ++depth;
                        opened = true;
                    } else if (c == '}') {
                        --depth;
                    }
                }
                if (opened && depth <= 0) break;
            }
            if (j >= lines.size()) j = lines.size() - 1;
            for (std::size_t k = i; k <= j; ++k) outside += lines[k] + "\n";
            i = j;
            continue;
        }
        inside += l + "\n";
    }
    if (trim(inside).empty()) return {};
    const std::string blanked = blank_literals(code);
    std::string out;
    out += includes_for(blank_literals(inside + outside), cpp);
    out += pre;
    if (cpp && blanked.find("std::") == std::string::npos &&
             (has_word(blanked, "cout") || has_word(blanked, "cin") || has_word(blanked, "endl") || has_word(blanked, "string") || has_word(blanked, "vector") ||
              has_word(blanked, "map") || has_word(blanked, "getline") || has_word(blanked, "sort")))
        out += "using namespace std;\n";
    out += "\n";
    if (!outside.empty()) out += outside + "\n";
    out += cpp ? "int main() {\n" : "int main(void) {\n";
    out += indent_block(inside, "    ");
    out += "    return 0;\n}\n";
    return out;
}

}  // namespace

std::vector<Candidate> candidates_for(const CodeBlock& block, bool prefer_c) {
    std::vector<Candidate> out;
    const std::string blanked = blank_literals(block.code);
    const bool main_there = has_main(blanked);
    std::vector<bool> langs;   // true: C++
    if (block.language == 1) langs = {false};
    else if (block.language == 2) langs = {true};
    else if (has_cpp_markers(blanked, block.code)) langs = {true};
    else if (prefer_c) langs = {false, true};
    else langs = {true, false};
    for (bool cpp : langs) {
        if (main_there) {
            // a program as written; an included header is turned into what the language wants only by the writer
            out.push_back({block.code + (block.code.back() == '\n' ? "" : "\n"), cpp});
        } else {
            std::string wrapped = wrap_statements(block.code, cpp);
            if (!wrapped.empty()) out.push_back({std::move(wrapped), cpp});
        }
    }
    return out;
}

// =====================================================================================================================
// Running a program for real
// =====================================================================================================================

namespace {

struct Run {
    bool good = false;       // built, ran, did not crash, finished, exit code 0
    CompileResult raw;
    std::string output;      // normalized
};

Run run_program(const std::string& code, bool cpp, const std::string& input) {
    Run r;
    r.raw = compile_and_run_lesson(code, cpp, input);
    r.good = r.raw.compiled && !r.raw.crashed && !r.raw.timed_out && r.raw.exit_code == 0;
    r.output = normalize_output(r.raw.output);
    return r;
}

// Output worth putting in a lesson: something, short, and made of ordinary text.
bool nice_output(const std::string& out) {
    if (out.empty() || out.size() > 700) return false;
    const std::vector<std::string> lines = split_lines(out);
    if (lines.size() > 10) return false;
    for (const std::string& l : lines) {
        if (l.size() > 100) return false;
        for (unsigned char c : l)
            if (c < 0x20 && c != '\t') return false;
    }
    // utf-8 that is not broken
    for (std::size_t i = 0; i < out.size();) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > out.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(out[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

const char* kTypedAnswers = "5\n3\n4\n";

std::string normalized_key(const std::string& code) {
    std::string k;
    for (unsigned char c : code)
        if (!std::isspace(c)) k += static_cast<char>(c);
    return k;
}

// The code lines of main's body: indexes into `lines` of the lines strictly between the braces. Empty when main is not found.
std::pair<int, int> main_body(const std::vector<std::string>& lines) {
    std::string flat;
    std::vector<std::size_t> starts;
    for (const std::string& l : lines) {
        starts.push_back(flat.size());
        flat += l + "\n";
    }
    const std::string b = blank_literals(flat);
    std::size_t at = 0;
    for (;; at += 4) {
        at = b.find("main", at);
        if (at == std::string::npos) return {-1, -1};
        if (at > 0 && is_ident(b[at - 1])) continue;
        std::size_t p = at + 4;
        while (p < b.size() && std::isspace(static_cast<unsigned char>(b[p]))) ++p;
        if (p < b.size() && b[p] == '(') break;
    }
    const std::size_t open = b.find('{', at);
    if (open == std::string::npos) return {-1, -1};
    int depth = 0;
    std::size_t close = std::string::npos;
    for (std::size_t i = open; i < b.size(); ++i) {
        if (b[i] == '{') ++depth;
        else if (b[i] == '}' && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos) return {-1, -1};
    auto line_of = [&](std::size_t pos) {
        int ln = 0;
        for (std::size_t i = 0; i < starts.size(); ++i)
            if (starts[i] <= pos) ln = static_cast<int>(i);
        return ln;
    };
    return {line_of(open) + 1, line_of(close)};
}

bool balanced_statement(const std::string& blanked_trimmed) {
    int paren = 0, brace = 0, bracket = 0;
    for (char c : blanked_trimmed) {
        paren += c == '(' ? 1 : c == ')' ? -1 : 0;
        brace += c == '{' ? 1 : c == '}' ? -1 : 0;
        bracket += c == '[' ? 1 : c == ']' ? -1 : 0;
    }
    return paren == 0 && brace == 0 && bracket == 0 && !blanked_trimmed.empty() && blanked_trimmed.back() == ';';
}

bool is_output_line(const std::string& raw_trimmed, const std::string& blanked_trimmed) {
    if (!balanced_statement(blanked_trimmed)) return false;
    return raw_trimmed.find("printf(") != std::string::npos || raw_trimmed.find("puts(") != std::string::npos || raw_trimmed.find("putchar(") != std::string::npos ||
           raw_trimmed.find("cout") != std::string::npos || raw_trimmed.find("printf (") != std::string::npos;
}

bool is_assignment_line(const std::string& blanked_trimmed) {
    if (!balanced_statement(blanked_trimmed)) return false;
    const std::string w = first_word(blanked_trimmed);
    if (w == "for" || w == "if" || w == "while" || w == "return" || w == "else" || w == "switch" || w == "do" || w == "break" || w == "continue" || w == "goto" || w == "typedef")
        return false;
    for (std::size_t i = 0; i < blanked_trimmed.size(); ++i) {
        if (blanked_trimmed[i] != '=') continue;
        const char before = i > 0 ? blanked_trimmed[i - 1] : ' ';
        const char after = i + 1 < blanked_trimmed.size() ? blanked_trimmed[i + 1] : ' ';
        if (after == '=' || before == '=' || before == '!' || before == '<' || before == '>') continue;
        return true;
    }
    return false;
}

std::string leading_space(const std::string& l) {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    return l.substr(0, i);
}

}  // namespace

Gap make_gap(const Program& p) {
    Gap g;
    std::vector<std::string> lines = split_lines(p.code);
    const std::vector<std::string> blanked = split_lines(blank_literals(p.code));
    const auto [from, to] = main_body(lines);
    if (from < 0 || to <= from) return g;
    std::vector<int> assigns, outputs;
    for (int i = from; i < to && i < static_cast<int>(lines.size()); ++i) {
        const std::string raw = trim(lines[static_cast<std::size_t>(i)]);
        const std::string bl = trim(blanked[static_cast<std::size_t>(i)]);
        if (raw.empty() || raw.size() > 90) continue;
        if (is_output_line(raw, bl)) outputs.push_back(i);
        else if (is_assignment_line(bl)) assigns.push_back(i);
    }
    // the middle of the program first
    auto by_middle = [&](std::vector<int>& v) {
        const int mid = (from + to) / 2;
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return std::abs(a - mid) < std::abs(b - mid); });
    };
    by_middle(assigns);
    by_middle(outputs);
    std::vector<int> order;
    for (std::size_t i = 0; i < assigns.size() && i < 2; ++i) order.push_back(assigns[i]);
    for (std::size_t i = 0; i < outputs.size() && i < 2; ++i) order.push_back(outputs[i]);
    for (std::size_t i = 2; i < assigns.size() && order.size() < 5; ++i) order.push_back(assigns[i]);
    for (int idx : order) {
        std::vector<std::string> gapped = lines;
        const std::string removed = trim(lines[static_cast<std::size_t>(idx)]);
        gapped[static_cast<std::size_t>(idx)] = leading_space(lines[static_cast<std::size_t>(idx)]) + "// ...";
        const std::string code = join_lines(gapped);
        const Run r = run_program(code, p.cpp, p.input);
        if (r.good && r.output == normalize_output(p.output)) continue;   // taking it out changes nothing: not a good gap
        g.ok = true;
        g.starter = code;
        g.line = removed;
        return g;
    }
    return g;
}

Change make_change(const Program& p) {
    Change c;
    std::vector<std::string> lines = split_lines(p.code);
    const std::vector<std::string> blanked = split_lines(blank_literals(p.code));
    const auto [from, to] = main_body(lines);
    if (from < 0 || to <= from) return c;
    struct Spot {
        int line;
        std::size_t at, len;
        int score;
        long long value;
    };
    std::vector<Spot> spots;
    for (int i = from; i < to && i < static_cast<int>(lines.size()); ++i) {
        const std::string& bl = blanked[static_cast<std::size_t>(i)];
        const std::string raw = trim(lines[static_cast<std::size_t>(i)]);
        const std::string bt = trim(bl);
        if (raw.empty() || raw[0] == '#' || first_word(bt) == "return" || raw.size() > 100) continue;
        const int score = is_assignment_line(bt) ? 0 : is_output_line(raw, bt) ? 1 : 2;
        for (std::size_t k = 0; k < bl.size();) {
            if (!is_digit(bl[k])) {
                ++k;
                continue;
            }
            std::size_t e = k;
            while (e < bl.size() && is_digit(bl[e])) ++e;
            const char before = k > 0 ? bl[k - 1] : ' ';
            const char after = e < bl.size() ? bl[e] : ' ';
            std::size_t pb = k;
            while (pb > 0 && bl[pb - 1] == ' ') --pb;
            const char prev_sig = pb > 0 ? bl[pb - 1] : ' ';
            if (!is_ident(before) && before != '.' && !is_ident(after) && after != '.' && prev_sig != '[' && e - k <= 5) {
                const long long v = std::atoll(bl.substr(k, e - k).c_str());
                if (v >= 1) spots.push_back({i, k, e - k, score, v});
            }
            k = e;
        }
    }
    std::stable_sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.score < b.score; });
    int tries = 0;
    for (const Spot& s : spots) {
        if (tries++ >= 5) break;
        const long long nv = s.value < 10 ? s.value + 3 : s.value + 10;
        std::vector<std::string> changed = lines;
        std::string& l = changed[static_cast<std::size_t>(s.line)];
        l = l.substr(0, s.at) + std::to_string(nv) + l.substr(s.at + s.len);
        const std::string code = join_lines(changed);
        const Run r = run_program(code, p.cpp, p.input);
        if (!r.good || !nice_output(r.output) || r.output == normalize_output(p.output)) continue;
        const Run again = run_program(code, p.cpp, p.input);
        if (!again.good || again.output != r.output) continue;
        c.ok = true;
        c.starter = p.code;
        c.solution = code;
        c.line = trim(lines[static_cast<std::size_t>(s.line)]);
        c.new_line = trim(l);
        c.from = std::to_string(s.value);
        c.to = std::to_string(nv);
        c.output = r.output;
        return c;
    }
    return c;
}

namespace {

// =====================================================================================================================
// Lessons
// =====================================================================================================================

struct LessonSpec {
    std::string kind;   // "run", "gap", "change"
    std::string title;
    std::vector<std::string> instructions;
    std::string task, starter, hint, expected, input;
    bool cpp = true;
};

std::string shorten(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t cut = std::string::npos;
    for (std::size_t i = max; i > max / 3; --i)
        if ((s[i - 1] == '.' || s[i - 1] == '!' || s[i - 1] == '?') && (i >= s.size() || s[i] == ' ')) {
            cut = i;
            break;
        }
    if (cut != std::string::npos) return trim(s.substr(0, cut));
    cut = s.rfind(' ', max);
    if (cut == std::string::npos || cut < max / 2) cut = max;
    std::string out = trim(s.substr(0, cut));
    // not in the middle of a UTF-8 letter
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
    return out + "\xE2\x80\xA6";
}

bool wordy(const std::string& p) {
    int words = 0;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (p[i] == ' ' && i > 0 && p[i - 1] != ' ') ++words;
    return p.size() >= 25 && words >= 4;
}

// The few paragraphs that go with a program: those just before it in the book, else the section's first ones.
std::vector<std::string> intro_for(const CodeBlock& b, const Section& s) {
    std::vector<std::string> src;
    for (const std::string& p : b.before)
        if (wordy(p) && !starts_with(p, "Listing") && !starts_with(p, "Figure")) src.push_back(p);
    if (src.empty() || b.whole_file)   // (an SDK sample: its own comment, then what the folder's documents say)
        for (const std::string& p : s.paragraphs)
            if (wordy(p) && src.size() < (b.whole_file ? 3u : 3u)) src.push_back(p);
    if (src.size() > 3 && !b.whole_file) src.erase(src.begin(), src.end() - 3);
    if (src.size() > 3) src.erase(src.begin(), src.end() - 3);
    std::vector<std::string> out;
    std::size_t budget = 760;
    for (const std::string& p : src) {
        if (budget < 80) break;
        const std::string t = shorten(p, std::min<std::size_t>(300, budget));
        out.push_back(t);
        budget -= std::min(budget, t.size());
    }
    if (out.empty()) out.push_back("This sample comes from \"" + short_title(s.title, 60) + "\".");
    return out;
}

std::string lesson_json(const LessonSpec& l, int number, const std::string& course_name) {
    std::ostringstream o;
    o << "{\n  \"title\": \"" << json_escape(l.title) << "\",\n";
    if (l.cpp) o << "  \"language\": \"cpp\",\n";
    o << "  \"number\": " << number << ",\n  \"book\": \"" << json_escape(course_name) << "\",\n  \"instructions\": [";
    for (std::size_t i = 0; i < l.instructions.size(); ++i) o << (i ? ",\n    " : "\n    ") << "\"" << json_escape(l.instructions[i]) << "\"";
    o << "\n  ],\n  \"task\": \"" << json_escape(l.task) << "\",\n  \"starter_code\": \"" << json_escape(l.starter) << "\",\n  \"hint\": \"" << json_escape(l.hint)
      << "\",\n  \"expected_output\": \"" << json_escape(l.expected) << "\",\n  \"input\": \"" << json_escape(l.input) << "\"\n}\n";
    return o.str();
}

// The three lessons a program can make, each already checked by really running it.
std::vector<LessonSpec> lessons_for(const Program& p, const CodeBlock& block, const Section& section) {
    std::vector<LessonSpec> out;
    // the program must give the same answer again, or it is not a lesson
    const Run again = run_program(p.code, p.cpp, p.input);
    if (!again.good || again.output != normalize_output(p.output)) return out;
    const std::vector<std::string> intro = intro_for(block, section);
    const std::string name = short_title(section.title);

    LessonSpec run;
    run.kind = "run";
    run.title = "Run: " + name;
    run.instructions = intro;
    run.instructions.push_back("Read the code, then press Run to see what it prints.");
    run.task = "Run it and see what it prints";
    run.starter = p.code;
    run.hint = "Just press Run. This program is already finished.";
    run.expected = normalize_output(p.output);
    run.input = p.input;
    run.cpp = p.cpp;
    out.push_back(run);

    const Gap g = make_gap(p);
    if (g.ok) {
        LessonSpec s;
        s.kind = "gap";
        s.title = "Gap: " + name;
        s.instructions = {"This is the same program, but one line has gone missing. It is marked with // ...",
                          "Read the code and work out what the missing line must do so that the program prints what is shown."};
        s.task = "Fill the gap so it prints exactly what is shown";
        s.starter = g.starter;
        s.hint = "The missing line is:\n\n    " + g.line;
        s.expected = run.expected;
        s.input = p.input;
        s.cpp = p.cpp;
        out.push_back(s);
    }
    const Change c = make_change(p);
    if (c.ok) {
        LessonSpec s;
        s.kind = "change";
        s.title = "Change: " + name;
        s.instructions = {"Now you change the program. Numbers in code are like dials: turn one, and the output changes.",
                          "Find this line:", "    " + c.line};
        s.task = "Change the " + c.from + " to " + c.to + " and run it. It should print what is shown";
        s.starter = c.starter;
        s.hint = "The line should become:\n\n    " + c.new_line;
        s.expected = c.output;
        s.input = p.input;
        s.cpp = p.cpp;
        out.push_back(s);
    }
    return out;
}

// =====================================================================================================================
// The whole job
// =====================================================================================================================

constexpr std::size_t kMaxLessons = 60;
constexpr std::size_t kMaxBlocks = 120;

struct Job {
    const ImportProgressFn& fn;
    std::mutex m;
    std::atomic<bool> cancelled{false};
    explicit Job(const ImportProgressFn& f) : fn(f) {}
    bool say(const std::string& s) {
        if (cancelled) return false;
        std::lock_guard<std::mutex> lock(m);
        if (fn && !fn(s)) cancelled = true;
        return !cancelled;
    }
};

template <class F>
void parallel_for(std::size_t count, unsigned threads, const F& fn) {
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const std::size_t i = next++;
            if (i >= count) return;
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads && t < count; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
}

unsigned worker_count() {
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp(n / 2, 2u, 4u);
}

ImportOutcome fail(const std::string& why) {
    ImportOutcome o;
    o.error = why;
    return o;
}

bool write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

ImportOutcome build_course(std::vector<Section> sections, const std::string& name, const std::string& source, const std::string& book_text_hint, Job& ctl) {
    // are the compiler and the tools there?
    {
        const Run probe = run_program("#include <stdio.h>\nint main(void) { printf(\"ok\"); return 0; }\n", false, "");
        if (!probe.good || probe.output != "ok") return fail("I can't build programs on this computer yet. Finish the setup on the Home page first.");
    }
    // the blocks worth trying
    struct Ref {
        std::size_t section, block;
    };
    std::vector<Ref> refs;
    bool any_cpp = has_cpp_markers(blank_literals(book_text_hint), book_text_hint), any_c = false;
    for (std::size_t s = 0; s < sections.size(); ++s)
        for (std::size_t b = 0; b < sections[s].blocks.size(); ++b) {
            const CodeBlock& blk = sections[s].blocks[b];
            if (!blk.whole_file && !plausible_code(blk.code)) continue;
            const std::string blanked = blank_literals(blk.code);
            if (unsafe_or_unsteady(blanked, blk.code)) continue;
            refs.push_back({s, b});
            if (blk.language == 2 || (blk.language == 0 && has_cpp_markers(blanked, blk.code))) any_cpp = true;
            if (blk.language == 1 || (blk.language == 0 && has_c_markers(blanked, blk.code))) any_c = true;
        }
    if (refs.empty()) return fail("I couldn't find any code samples. Books where the code is in a typewriter-style font work best.");
    const bool prefer_c = any_c && !any_cpp;
    if (refs.size() > kMaxBlocks) {   // spread the tries over the whole book
        std::vector<Ref> thin;
        for (std::size_t i = 0; i < kMaxBlocks; ++i) thin.push_back(refs[i * refs.size() / kMaxBlocks]);
        refs.swap(thin);
    }

    // ---- 1. which code samples really build and run
    std::vector<std::optional<Program>> found(refs.size());
    std::atomic<std::size_t> tried{0};
    const auto started = std::chrono::steady_clock::now();
    parallel_for(refs.size(), worker_count(), [&](std::size_t i) {
        if (ctl.cancelled) return;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(8)) return;   // a limit on the whole job
        const CodeBlock& blk = sections[refs[i].section].blocks[refs[i].block];
        const std::string input = reads_input(blk.code) ? kTypedAnswers : "";
        for (const Candidate& c : candidates_for(blk, prefer_c)) {
            if (ctl.cancelled) break;
            const Run r = run_program(c.code, c.cpp, input);
            if (r.good && nice_output(r.output)) {
                Program p;
                p.code = c.code;
                p.cpp = c.cpp;
                p.input = input;
                p.output = r.output;
                p.section = refs[i].section;
                p.block = refs[i].block;
                found[i] = std::move(p);
                break;
            }
        }
        ctl.say("Trying code sample " + std::to_string(++tried) + " of " + std::to_string(refs.size()));
    });
    if (ctl.cancelled) {
        ImportOutcome o;
        o.cancelled = true;
        o.error = "Stopped.";
        return o;
    }
    std::vector<Program> programs;
    std::unordered_set<std::string> seen;
    for (std::optional<Program>& f : found)
        if (f && seen.insert(normalized_key(f->code)).second) programs.push_back(std::move(*f));
    if (programs.empty()) return fail("None of the code samples in it would build and run by themselves, so there was nothing to make lessons from.");

    // ---- 2. a few lessons from each, spread over the book
    const std::size_t want = (kMaxLessons + 2) / 3;
    if (programs.size() > want) {
        std::vector<Program> thin;
        for (std::size_t i = 0; i < want; ++i) thin.push_back(std::move(programs[i * programs.size() / want]));
        programs.swap(thin);
    }
    std::vector<std::vector<LessonSpec>> made(programs.size());
    std::atomic<std::size_t> done{0};
    parallel_for(programs.size(), worker_count(), [&](std::size_t i) {
        if (ctl.cancelled) return;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(14)) return;
        const Program& p = programs[i];
        made[i] = lessons_for(p, sections[p.section].blocks[p.block], sections[p.section]);
        ctl.say("Making lessons: program " + std::to_string(++done) + " of " + std::to_string(programs.size()));
    });
    if (ctl.cancelled) {
        ImportOutcome o;
        o.cancelled = true;
        o.error = "Stopped.";
        return o;
    }
    std::vector<LessonSpec> all;
    int kept_programs = 0;
    bool any_c_lesson = false, any_cpp_lesson = false;
    for (std::vector<LessonSpec>& set : made) {
        if (set.empty() || all.size() + set.size() > kMaxLessons + 2) continue;
        ++kept_programs;
        for (LessonSpec& l : set) {
            (l.cpp ? any_cpp_lesson : any_c_lesson) = true;
            all.push_back(std::move(l));
        }
    }
    if (all.size() > kMaxLessons) all.resize(kMaxLessons);
    if (all.empty()) return fail("The code samples built, but none of them gave the same answer twice, so I made no lessons.");

    // ---- 3. save
    if (!ctl.say("Saving the lessons…")) {
        ImportOutcome stopped;
        stopped.cancelled = true;
        stopped.error = "Stopped.";
        return stopped;
    }
    const fs::path root = courses_root();
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string folder = safe_name(name);
    for (int n = 2; fs::exists(root / folder, ec); ++n) folder = safe_name(name) + "_" + std::to_string(n);
    const fs::path tmp = root / (folder + ".making");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    int number = 0;
    for (const LessonSpec& l : all) {
        ++number;
        char file[64];
        std::snprintf(file, sizeof file, "%02d_%s.json", number, l.kind.c_str());
        if (!write_file(tmp / file, lesson_json(l, number, name))) {
            fs::remove_all(tmp, ec);
            return fail("I couldn't save the lessons. Is the disk full?");
        }
    }
    const std::string language = any_c_lesson && any_cpp_lesson ? "C and C++" : any_c_lesson ? "C" : "C++";
    std::ostringstream course;
    course << "{\n  \"name\": \"" << json_escape(name) << "\",\n  \"source\": \"" << json_escape(source) << "\",\n  \"language\": \"" << language << "\",\n  \"date\": \""
           << today_string() << "\",\n  \"lessons\": " << all.size() << ",\n  \"programs\": " << kept_programs << "\n}\n";
    if (!write_file(tmp / "course.json", course.str())) {
        fs::remove_all(tmp, ec);
        return fail("I couldn't save the lessons. Is the disk full?");
    }
    fs::rename(tmp, root / folder, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        return fail("I couldn't save the lessons.");
    }
    ImportOutcome o;
    o.ok = true;
    o.folder = folder;
    o.name = name;
    o.lessons = static_cast<int>(all.size());
    o.programs = kept_programs;
    return o;
}

std::string book_name_from(const std::string& title, const fs::path& file) {
    std::string t = collapse_spaces(title);
    std::string lower = t;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t.size() >= 3 && lower != "untitled" && lower.find("microsoft word") == std::string::npos && !starts_with(lower, "untitled")) return shorten(t, 70);
    std::string stem = path_utf8(file.stem());
    for (char& c : stem)
        if (c == '_') c = ' ';
    return shorten(collapse_spaces(stem), 70);
}

}  // namespace

std::filesystem::path courses_root() {
    if (!courses_override().empty()) return courses_override();
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = fs::path(base) / L"CppSparring" / L"courses";
    CoTaskMemFree(base);
    return dir;
}

void use_courses_dir(const std::wstring& path) { courses_override() = path; }

std::vector<ImportedCourse> imported_courses() {
    std::vector<ImportedCourse> out;
    std::error_code ec;
    const fs::path root = courses_root();
    if (root.empty() || !fs::exists(root, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string folder = path_utf8(e.path().filename());
        if (folder.size() > 7 && folder.compare(folder.size() - 7, 7, ".making") == 0) continue;
        const std::string text = read_text_file(e.path() / "course.json", 64 * 1024);
        if (text.empty()) continue;
        json::JParser parser(text);
        const json::JValue v = parser.parse();
        if (v.type != json::JType::Object) continue;
        ImportedCourse c;
        c.folder = folder;
        c.name = v.text("name");
        c.source = v.text("source");
        c.language = v.text("language");
        c.date = v.text("date");
        if (const json::JValue* n = v.find("lessons")) c.lessons = static_cast<int>(n->num);
        if (const json::JValue* n = v.find("programs")) c.programs = static_cast<int>(n->num);
        if (c.name.empty()) c.name = folder;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const ImportedCourse& a, const ImportedCourse& b) { return a.folder < b.folder; });
    return out;
}

bool remove_imported_course(const std::string& folder) {
    if (folder.empty() || folder.find_first_of("/\\:") != std::string::npos || folder == "." || folder == "..") return false;
    std::error_code ec;
    const fs::path dir = courses_root() / path_from_utf8(folder);
    if (!fs::exists(dir / "course.json", ec)) return false;   // only what this program made
    fs::remove_all(dir, ec);
    return !ec;
}

ImportOutcome import_pdf(const std::wstring& path, const ImportProgressFn& progress) {
    Job ctl(progress);
    const fs::path file(path);
    ctl.say("Opening the PDF…");
    const pdf::Document doc = pdf::read_file(path, [&](int page, int total) {
        return ctl.say("Reading page " + std::to_string(std::min(page + 1, total)) + " of " + std::to_string(total));
    });
    if (doc.cancelled || ctl.cancelled) {
        ImportOutcome o;
        o.cancelled = true;
        o.error = "Stopped.";
        return o;
    }
    if (!doc.ok) return fail(doc.error);
    ctl.say("Looking for code samples…");
    std::vector<Section> sections = sections_from_pdf(doc);
    std::string hint;
    for (const Section& s : sections)
        for (const CodeBlock& b : s.blocks) hint += b.code + "\n";
    return build_course(std::move(sections), book_name_from(doc.title, file), path_utf8(file.filename()), hint, ctl);
}

ImportOutcome import_folder(const std::wstring& path, const ImportProgressFn& progress) {
    Job ctl(progress);
    const fs::path dir(path);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return fail("I couldn't open that folder.");
    ctl.say("Looking through the folder…");
    std::vector<Section> sections = sections_from_folder(dir);
    if (ctl.cancelled) {
        ImportOutcome o;
        o.cancelled = true;
        o.error = "Stopped.";
        return o;
    }
    if (sections.empty()) return fail("I couldn't find any C or C++ programs (with a main job) in that folder.");
    std::string name = path_utf8(dir.filename().empty() ? dir.parent_path().filename() : dir.filename());
    name = prettify_folder(name);
    return build_course(std::move(sections), name, path_utf8(dir.filename().empty() ? dir : dir.filename()), "", ctl);
}

int import_cli(const std::wstring& input, const std::wstring& courses_dir, const std::wstring& log_path) {
    use_courses_dir(courses_dir);
    std::ofstream log{fs::path(log_path), std::ios::binary};
    std::error_code ec;
    const bool folder = fs::is_directory(fs::path(input), ec);
    std::vector<Section> sections;
    if (folder) {
        sections = sections_from_folder(fs::path(input));
    } else {
        const pdf::Document doc = pdf::read_file(input);
        if (!doc.ok) {
            log << "READ FAILED: " << doc.error << "\n";
            return 1;
        }
        log << "title: " << doc.title << ", pages: " << doc.pages.size() << "\n";
        sections = sections_from_pdf(doc);
    }
    for (const Section& s : sections) {
        log << "== SECTION \"" << s.title << "\": " << s.paragraphs.size() << " paragraphs, " << s.blocks.size() << " code blocks\n";
        for (const CodeBlock& b : s.blocks) {
            log << "-- block (page " << b.page << ")\n" << b.code << "\n";
            for (const std::string& p : b.before) log << "   before: " << p << "\n";
        }
    }
    const ImportOutcome o = folder ? import_folder(input, [&](const std::string& s) {
        log << "progress: " << s << "\n";
        return true;
    }) : import_pdf(input, [&](const std::string& s) {
        log << "progress: " << s << "\n";
        return true;
    });
    log << "RESULT: ok=" << o.ok << " lessons=" << o.lessons << " programs=" << o.programs << " folder=" << o.folder << " name=" << o.name << " error=" << o.error << "\n";
    return o.ok ? 0 : 1;
}

}  // namespace sparring
