#include "replay.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

#include "compile.hpp"
#include "explain.hpp"

namespace sparring {

namespace {

constexpr int kMaxSteps = 400;

const char* kHeader = R"(#include <iostream>
#include <sstream>
#include <string>
namespace spar_trace {
inline int count = 0;
template <class T> std::string show(const T& v) { std::ostringstream s; s << v; return s.str(); }
inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(char v) { return std::string("'") + v + "'"; }
inline void step(int line, const char* fn) { if (++count > 400) return; std::cout << '\x1E' << line << '\x1D' << fn; }
template <class T> void v(const char* n, const T& x) { if (count > 400) return; std::cout << '\x1D' << n << '\x1C' << show(x); }
inline void end() { if (count > 400) return; std::cout << '\x1F'; }
}
)";

std::vector<std::string> split_keep(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool starts_with_word(const std::string& s, const char* w) {
    const std::string word(w);
    return s.compare(0, word.size(), word) == 0 && (s.size() == word.size() || !(std::isalnum(static_cast<unsigned char>(s[word.size()])) || s[word.size()] == '_'));
}

// "const std::string &" -> "std::string"
std::string bare_type(std::string t) {
    for (const char* junk : {"const ", "struct ", "class ", "&"}) {
        const std::string j(junk);
        for (std::size_t p = t.find(j); p != std::string::npos; p = t.find(j)) t.erase(p, j.size());
    }
    return trim(t);
}

bool printable(const std::string& t) {
    static const std::set<std::string> ok = {"int", "long", "short", "long long", "unsigned int", "unsigned", "unsigned long", "size_t", "std::size_t",
                                             "double", "float", "bool", "char", "std::string", "string"};
    return ok.count(t) > 0;
}

bool is_function(const std::string& k) { return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl"; }

int paren_balance(const std::string& s) {
    int b = 0;
    for (char c : s) b += c == '(' ? 1 : c == ')' ? -1 : 0;
    return b;
}

}  // namespace

std::string instrument(const std::string& source, const std::vector<AstNode>& nodes) {
    const std::vector<std::string> lines = split_keep(source);
    const int count = static_cast<int>(lines.size());
    auto text = [&](int line) { return line >= 1 && line <= count ? trim(lines[static_cast<std::size_t>(line - 1)]) : std::string(); };
    auto prev_code = [&](int line) {
        for (int l = line - 1; l >= 1; --l)
            if (!text(l).empty()) return text(l);
        return std::string();
    };
    auto next_code = [&](int line) {
        for (int l = line + 1; l <= count; ++l)
            if (!text(l).empty()) return text(l);
        return std::string();
    };

    // the boxes a blueprint holds that can be shown
    std::map<std::string, std::vector<std::string>> fields;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].kind != "CXXRecordDecl" || nodes[i].name.empty()) continue;
        for (std::size_t j = i + 1; j < nodes.size() && nodes[j].kind != "CXXRecordDecl" && !is_function(nodes[j].kind); ++j)
            if (nodes[j].kind == "FieldDecl" && printable(bare_type(nodes[j].type))) fields[nodes[i].name].push_back(nodes[j].name);
    }

    // where to report: after a line (true) or before it (false), with the boxes that can be seen there
    std::map<int, std::string> after, before;
    for (const AstNode& f : nodes) {
        if (!is_function(f.kind) || f.end_line <= f.line) continue;
        if (text(f.line).find(';') != std::string::npos && text(f.line).find('{') == std::string::npos) continue;   // only a declaration
        for (int line = f.line; line < f.end_line; ++line) {
            const std::string s = text(line);
            if (s.empty() || s[0] == '#' || starts_with_word(s, "case") || starts_with_word(s, "default")) continue;
            bool put_after = false, put_before = false;
            if (s.back() == '{') {
                std::string head = trim(s.substr(0, s.size() - 1));
                if (head.empty()) head = prev_code(line);   // a { on a line of its own belongs to the line before
                const bool opens_body = line == f.line || (!head.empty() && head.back() == ')') || head == "else" || head == "} else" ||
                                        head == "do" || starts_with_word(head, "try");
                put_after = opens_body && !starts_with_word(s, "switch") && s.find('[') == std::string::npos;
            } else if (s.back() == ';' && paren_balance(s) == 0) {
                if (starts_with_word(s, "return") || starts_with_word(s, "break") || starts_with_word(s, "continue") || starts_with_word(s, "throw")) {
                    const std::string p = prev_code(line);
                    put_before = !(p.empty() || p.back() == ')' || p == "else" || p == "} else");
                } else {
                    put_after = !starts_with_word(next_code(line), "else");
                }
            }
            if (!put_after && !put_before) continue;

            std::string call = "spar_trace::step(" + std::to_string(line) + ", \"" + f.name + "\");";
            for (const AstNode& v : nodes) {
                if ((v.kind != "VarDecl" && v.kind != "ParmVarDecl") || v.name.empty()) continue;
                const bool global = v.function.empty() && v.kind == "VarDecl" && (v.line < f.line || v.line > f.end_line);
                const bool mine = v.function == f.name && v.line >= f.line && v.line <= f.end_line;
                if (!global && !mine) continue;
                const bool seen = put_after ? (v.line <= line && v.scope_end > line) : (v.line < line && v.scope_end >= line);
                if (!seen) continue;
                const std::string t = bare_type(v.type);
                if (printable(t)) {
                    call += " spar_trace::v(\"" + v.name + "\", " + v.name + ");";
                } else if (auto it = fields.find(t); it != fields.end()) {
                    for (const std::string& fld : it->second) call += " spar_trace::v(\"" + v.name + "." + fld + "\", " + v.name + "." + fld + ");";
                }
            }
            call += " spar_trace::end();";
            (put_after ? after : before)[line] = call;
        }
    }

    std::string out = kHeader;
    for (int line = 1; line <= count; ++line) {
        if (auto it = before.find(line); it != before.end()) out += it->second + "\n";
        out += lines[static_cast<std::size_t>(line - 1)] + "\n";
        if (auto it = after.find(line); it != after.end()) out += it->second + "\n";
    }
    return out;
}

Replay record_replay(const std::string& source, const std::vector<AstNode>& nodes, const std::string& original_path, const std::string& input) {
    Replay r;
    r.source = source;
    const CompileResult run = compile_and_run_string(instrument(source, nodes), original_path, input);
    if (!run.compiled) {
        r.error = plain_error(run.compile_errors);
        return r;
    }
    r.ok = true;
    r.crashed = run.crashed;
    r.timed_out = run.timed_out;

    std::string printed;
    const std::string& s = run.output;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '\x1E') {
            printed += s[i++];
            continue;
        }
        const std::size_t end = s.find('\x1F', i);
        if (end == std::string::npos) break;   // cut off by a crash
        const std::string rec = s.substr(i + 1, end - i - 1);
        i = end + 1;
        std::vector<std::string> parts;
        std::size_t p = 0;
        for (std::size_t q = rec.find('\x1D'); q != std::string::npos; q = rec.find('\x1D', p)) {
            parts.push_back(rec.substr(p, q - p));
            p = q + 1;
        }
        parts.push_back(rec.substr(p));
        if (parts.size() < 2) continue;
        ReplayStep step;
        step.line = std::atoi(parts[0].c_str());
        step.function = parts[1];
        for (std::size_t k = 2; k < parts.size(); ++k) {
            const std::size_t sep = parts[k].find('\x1C');
            if (sep != std::string::npos) step.vars.push_back({parts[k].substr(0, sep), parts[k].substr(sep + 1)});
        }
        step.printed = printed;
        r.steps.push_back(std::move(step));
    }
    r.output = printed;
    r.cut_short = static_cast<int>(r.steps.size()) >= kMaxSteps;
    return r;
}

}  // namespace sparring
