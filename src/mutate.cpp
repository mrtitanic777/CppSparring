#include "mutate.hpp"

#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace sparring {

namespace {

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

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Where `pat` first appears on `s` outside "..." and '...'; npos when it does not.
std::size_t find_code(const std::string& s, const std::string& pat) {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') return std::string::npos;
        if (s.compare(i, pat.size(), pat) == 0) return i;
    }
    return std::string::npos;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// The operators a beginner can swap, what they turn into, and why it matters.
struct Swap {
    const char* from;
    const char* to;
    const char* why;
};
const Swap kSwaps[] = {
    {"+", "-", "+ adds things together. - takes away instead."},
    {"-", "+", "- takes away. + adds instead."},
    {"*", "+", "* multiplies (times). + only adds."},
    {"<", "<=", "< means \"smaller than\". <= means \"smaller than, or the same\"."},
    {"<=", "<", "<= means \"smaller than, or the same\". < means only \"smaller than\"."},
    {">", ">=", "> means \"bigger than\". >= means \"bigger than, or the same\"."},
    {">=", ">", ">= means \"bigger than, or the same\". > means only \"bigger than\"."},
};

std::size_t find_word(const std::string& s, const std::string& word, std::size_t from = 0) {
    if (word.empty()) return std::string::npos;
    for (std::size_t p = s.find(word, from); p != std::string::npos; p = s.find(word, p + 1)) {
        const bool left = p == 0 || !ident_char(s[p - 1]);
        const bool right = p + word.size() >= s.size() || !ident_char(s[p + word.size()]);
        if (left && right) return p;
    }
    return std::string::npos;
}

void change_line(std::string& s, const MutationToggle& t) {
    switch (t.rule) {
    case Rule::RemoveRef:
        for (std::size_t p = find_word(s, t.name); p != std::string::npos; p = find_word(s, t.name, p + 1)) {
            std::size_t q = p;
            while (q > 0 && s[q - 1] == ' ') --q;
            if (q > 0 && s[q - 1] == '&' && (q < 2 || s[q - 2] != '&')) {
                s.erase(q - 1, 1);
                break;
            }
        }
        break;
    case Rule::RemoveConst: {
        const std::size_t name_at = find_word(s, t.name);
        std::size_t c = std::string::npos;
        for (std::size_t p = find_word(s, "const"); p != std::string::npos && p < name_at; p = find_word(s, "const", p + 1)) c = p;
        if (c != std::string::npos) {
            std::size_t e = c + 5;
            while (e < s.size() && s[e] == ' ') ++e;
            s.erase(c, e - c);
        }
        break;
    }
    case Rule::RemoveReturn:
    case Rule::RemoveIgnore: {
        std::size_t indent = 0;
        while (indent < s.size() && (s[indent] == ' ' || s[indent] == '\t')) ++indent;
        s.insert(indent, "// ");
        break;
    }
    case Rule::EqToAssign: {
        const std::size_t p = find_code(s, "==");
        if (p != std::string::npos) s.replace(p, 2, "=");
        break;
    }
    case Rule::SwapOp: {
        const std::string pat = " " + t.op_from + " ";
        const std::size_t p = find_code(s, pat);
        if (p != std::string::npos) s.replace(p, pat.size(), " " + t.op_to + " ");
        break;
    }
    case Rule::RemoveAssign: {
        std::size_t indent = 0;
        while (indent < s.size() && (s[indent] == ' ' || s[indent] == '\t')) ++indent;
        s.insert(indent, "// ");
        break;
    }
    }
}

std::string tidy(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '\t') out += "    ";
        else if (c != '\r') out += c;
    }
    return out;
}

void add(std::vector<MutationToggle>& out, std::set<std::string>& seen, Rule rule, const AstNode& n, int index, const std::string& name,
         std::string label, std::string description) {
    if (!seen.insert(label + "@" + std::to_string(n.line)).second) return;
    MutationToggle t;
    t.rule = rule;
    t.label = std::move(label);
    t.description = std::move(description);
    t.name = name;
    t.line = n.line;
    t.target_node = index;
    out.push_back(std::move(t));
}

}  // namespace

std::vector<MutationToggle> generate_mutations(const std::vector<AstNode>& nodes, const std::string& source) {
    std::vector<MutationToggle> toggles;
    std::set<std::string> seen;
    const std::vector<std::string> lines = split_keep(source);
    auto line_text = [&](int line) { return line >= 1 && line <= static_cast<int>(lines.size()) ? lines[static_cast<std::size_t>(line - 1)] : std::string(); };
    std::string function;   // the job the node is inside (nodes come in the order they are written)
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        const AstNode& n = nodes[static_cast<std::size_t>(i)];
        if (n.kind == "FunctionDecl" || n.kind == "CXXMethodDecl" || n.kind == "CXXConstructorDecl") function = n.name;
        // an operator between two values; on text and lists it is an overloaded one ("==" on two strings), the same to the learner
        const bool op = n.kind == "BinaryOperator" || n.kind == "CXXOperatorCallExpr";
        const bool compound = n.kind == "CompoundAssignOperator" ||
                              (n.kind == "CXXOperatorCallExpr" && n.name.size() == 2 && n.name[1] == '=' && n.name != "==" && n.name != "!=" &&
                               n.name != "<=" && n.name != ">=");
        if (op) {
            for (const Swap& sw : kSwaps) {
                if (n.name != sw.from || find_code(line_text(n.line), std::string(" ") + sw.from + " ") == std::string::npos) continue;
                const std::size_t before = toggles.size();
                add(toggles, seen, Rule::SwapOp, n, i, n.name, std::string("Change ") + sw.from + " into " + sw.to, sw.why);
                if (toggles.size() > before) {
                    toggles.back().op_from = sw.from;
                    toggles.back().op_to = sw.to;
                }
            }
        }
        // a line that only changes a box: "age = age + 1;" or "total += i;"
        if ((op && n.name == "=") || compound) {
            const std::string s = trim(line_text(n.line));
            const std::size_t eq = find_code(s, n.name == "=" ? " = " : " " + n.name + " ");
            if (eq != std::string::npos && !s.empty() && s.back() == ';' && (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
                s.find('(') == std::string::npos) {
                const std::string target = trim(s.substr(0, eq));
                add(toggles, seen, Rule::RemoveAssign, n, i, target, "Delete the line that changes " + target,
                    "This line puts a new value into " + target + ". We are deleting it, so " + target + " keeps its old value.");
            }
        }
        if (n.kind == "ParmVarDecl" && !n.name.empty() && n.type.find('&') != std::string::npos && n.type.find("&&") == std::string::npos) {
            add(toggles, seen, Rule::RemoveRef, n, i, n.name, "Take away the & from " + n.name,
                "With &, the job works on the real " + n.name + ". Without it, the job gets a copy, like doing homework on a photocopy instead of the real sheet.");
        }
        if ((n.kind == "ParmVarDecl" || n.kind == "VarDecl") && !n.name.empty() && n.type.rfind("const ", 0) == 0) {
            add(toggles, seen, Rule::RemoveConst, n, i, n.name, "Take away const from " + n.name,
                "const is a promise that says: this will never change. We are taking the promise away.");
        }
        if (n.kind == "ReturnStmt") {
            add(toggles, seen, Rule::RemoveReturn, n, i, function, "Delete the return in " + function,
                "return hands the job's answer back to whoever asked for it. We are deleting it.");
        }
        if (n.kind == "MemberExpr" && n.name == "ignore") {
            add(toggles, seen, Rule::RemoveIgnore, n, i, n.name, "Delete cin.ignore()",
                "cin.ignore() throws away the leftover Enter key after you type something. We are deleting it.");
        }
        if (op && n.name == "==") {
            add(toggles, seen, Rule::EqToAssign, n, i, n.name, "Change == into =",
                "== asks a question: are these two the same? A single = doesn't ask anything. It copies the right side into the left box (read it right to left).");
        }
    }
    return toggles;
}

std::string apply_mutation(const std::string& source, const MutationToggle& t) {
    std::vector<std::string> lines = split_keep(source);
    if (t.line >= 1 && t.line <= static_cast<int>(lines.size())) change_line(lines[static_cast<std::size_t>(t.line - 1)], t);
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += '\n';
    }
    return out;
}

std::string line_before(const std::string& source, const MutationToggle& t) {
    const std::vector<std::string> lines = split_keep(source);
    if (t.line < 1 || t.line > static_cast<int>(lines.size())) return {};
    return tidy(lines[static_cast<std::size_t>(t.line - 1)]);
}

std::string line_after(const std::string& source, const MutationToggle& t) {
    std::vector<std::string> lines = split_keep(source);
    if (t.line < 1 || t.line > static_cast<int>(lines.size())) return {};
    std::string s = lines[static_cast<std::size_t>(t.line - 1)];
    change_line(s, t);
    return tidy(s);
}

}  // namespace sparring
