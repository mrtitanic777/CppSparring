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
    case Rule::SwapOp:
    case Rule::PastEnd: {
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
    case Rule::StartAt: {
        const std::string pat = " " + t.name + " = " + t.op_from + ";";
        const std::size_t p = find_code(s, pat);
        if (p != std::string::npos) s.replace(p, pat.size(), " " + t.name + " = " + t.op_to + ";");
        break;
    }
    case Rule::MakeInt: {
        const std::size_t name_at = find_word(s, t.name);
        std::size_t c = std::string::npos;
        for (std::size_t p = find_word(s, t.op_from); p != std::string::npos && p < name_at; p = find_word(s, t.op_from, p + 1)) c = p;
        if (c != std::string::npos) s.replace(c, t.op_from.size(), "int");
        break;
    }
    case Rule::RemoveInit: {
        const std::size_t p = find_code(s, " " + t.name + " = ");
        const std::size_t end = s.rfind(';');
        if (p != std::string::npos && end != std::string::npos && end > p) {
            const std::size_t from = p + 1 + t.name.size();
            s.erase(from, end - from);
        }
        break;
    }
    case Rule::WholeDivide: {
        const std::string pat = " / " + t.op_from;
        const std::size_t p = find_code(s, pat);
        if (p != std::string::npos) s.replace(p, pat.size(), " / " + t.op_to);
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
                // a loop over a list: < its size, turned into <=, reads one box past the end
                if (n.name == "<" && find_code(line_text(n.line), ".size()") != std::string::npos)
                    add(toggles, seen, Rule::PastEnd, n, i, n.name, "Let the loop go one box past the end",
                        "A list counts its boxes from 0, so a list of 4 has boxes 0, 1, 2 and 3. With <= the loop also tries box 4, which isn't there.");
                else
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
        // dividing by a number with a dot (4.0): without the dot it is whole-number division
        if (op && n.name == "/") {
            const std::string s = line_text(n.line);
            const std::size_t p = find_code(s, " / ");
            if (p != std::string::npos) {
                std::size_t e = p + 3, dot = std::string::npos;
                while (e < s.size() && (std::isdigit(static_cast<unsigned char>(s[e])) || s[e] == '.')) {
                    if (s[e] == '.') dot = e;
                    ++e;
                }
                const std::string num = s.substr(p + 3, e - p - 3);
                bool zeros = dot != std::string::npos && dot > p + 3 && dot + 1 < e;
                for (std::size_t k = dot + 1; zeros && k < e; ++k) zeros = s[k] == '0';
                if (zeros) {
                    const std::string whole = s.substr(p + 3, dot - p - 3);
                    add(toggles, seen, Rule::WholeDivide, n, i, n.name, "Change " + num + " into " + whole,
                        "With " + num + " the computer keeps the part after the dot. With just " + whole +
                            ", both sides are whole numbers, so it does whole-number division: 7 / 2 is 3, not 3.5.");
                    if (!toggles.empty() && toggles.back().rule == Rule::WholeDivide && toggles.back().line == n.line) {
                        toggles.back().op_from = num;
                        toggles.back().op_to = whole;
                    }
                }
            }
        }
        if ((n.kind == "VarDecl" || n.kind == "ParmVarDecl") && !n.name.empty() && (n.type == "double" || n.type == "float") &&
            find_word(line_text(n.line), n.type) != std::string::npos) {
            add(toggles, seen, Rule::MakeInt, n, i, n.name, "Make " + n.name + " a whole number (int)",
                std::string("A ") + n.type + " box holds numbers with a dot, like 3.5. An int box only holds whole numbers, so the part after the dot gets chopped off.");
            if (!toggles.empty() && toggles.back().rule == Rule::MakeInt && toggles.back().line == n.line) toggles.back().op_from = n.type;
        }
        if (n.kind == "VarDecl" && !n.name.empty()) {
            const std::string s = trim(line_text(n.line));
            const bool in_for = find_code(s, "for (") != std::string::npos;
            if (in_for) {   // the loop's counter: start it one off
                for (const char* from : {"0", "1"}) {
                    if (find_code(s, " " + n.name + " = " + from + ";") == std::string::npos) continue;
                    const std::string to = from[0] == '0' ? "1" : "0";
                    add(toggles, seen, Rule::StartAt, n, i, n.name, "Start " + n.name + " at " + to + " instead of " + from,
                        "This loop counts with " + n.name + ". Starting it at " + to + " makes the loop run one time " +
                            (to == "1" ? "fewer (and skip box 0 of a list)." : "more."));
                    if (!toggles.empty() && toggles.back().rule == Rule::StartAt && toggles.back().line == n.line) {
                        toggles.back().op_from = from;
                        toggles.back().op_to = to;
                    }
                }
            }
            // "int total = 0;": a whole-number box with a first value, alone on its line
            // (whole numbers only: their junk is always -1431655766, where a double's is a puzzling "nan")
            static const std::set<std::string> numbers = {"int", "long", "long long", "short"};
            if (!in_for && numbers.count(n.type) && !s.empty() && s.back() == ';' && s.find(',') == std::string::npos &&
                s.rfind("const", 0) != 0 && s.rfind("static", 0) != 0 && find_code(s, " " + n.name + " = ") != std::string::npos) {
                add(toggles, seen, Rule::RemoveInit, n, i, n.name, "Leave out " + n.name + "'s first value",
                    "This line gives " + n.name + " its first value. Without it, the box holds whatever junk was left in that spot of memory.");
            }
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
