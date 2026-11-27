#include "teacher.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <vector>

#include "explain.hpp"

namespace sparring {

namespace {

std::vector<std::string> lines_of(const std::string& code) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : code) {
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

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The line without a // note at its end (a // inside quotes is kept).
std::string code_part(const std::string& s) {
    bool quote = false;
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) quote = !quote;
        if (!quote && s[i] == '/' && s[i + 1] == '/') return trim(s.substr(0, i));
    }
    return trim(s);
}

// "std::vector<int>" -> "a list of whole numbers"
std::string type_words(std::string t) {
    t = trim(t);
    if (starts(t, "const ")) return type_words(t.substr(6)) + " that can never change";
    if (starts(t, "std::")) t = t.substr(5);
    if (t == "int" || t == "long" || t == "long long" || t == "short" || t == "unsigned" || t == "unsigned int") return "a whole number";
    if (t == "double" || t == "float") return "a number with a decimal point";
    if (t == "string") return "words";
    if (t == "char") return "one letter";
    if (t == "bool") return "true or false";
    if (t == "size_t") return "a counting number";
    if (t == "auto") return "a value";
    if (starts(t, "vector<")) {
        const std::string inner = t.substr(7, t.size() > 8 ? t.size() - 8 : 0);
        const std::string w = type_words(inner);
        if (w == "a whole number") return "a list of whole numbers";
        if (w == "a number with a decimal point") return "a list of decimal numbers";
        if (w == "words") return "a list of words";
        return "a list";
    }
    return "a " + t;
}

// A question in code, said in words: "i < scores.size()" -> "i is smaller than the size of scores".
std::string condition_words(std::string c) {
    c = trim(c);
    static const std::pair<std::regex, const char*> said[] = {
        {std::regex(R"((\w+)\.size\(\))"), "the size of $1"},
        {std::regex(R"(\s*==\s*)"), " is the same as "},
        {std::regex(R"(\s*!=\s*)"), " is not "},
        {std::regex(R"(\s*<=\s*)"), " is smaller than or the same as "},
        {std::regex(R"(\s*>=\s*)"), " is bigger than or the same as "},
        {std::regex(R"(\s*<\s*)"), " is smaller than "},
        {std::regex(R"(\s*>\s*)"), " is bigger than "},
        {std::regex(R"(\s*&&\s*)"), ", and "},
        {std::regex(R"(\s*\|\|\s*)"), ", or "},
        {std::regex(R"(!(\w))"), "not $1"},
        {std::regex(R"(\s*%\s*)"), " left over after dividing by "},
    };
    for (const auto& [re, words] : said) c = std::regex_replace(c, re, words);
    return c;
}

// A single = inside a question (not ==, <=, >= or !=).
bool lone_equals(const std::string& c) {
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (c[i] != '=') continue;
        const char before = i ? c[i - 1] : ' ';
        const char after = i + 1 < c.size() ? c[i + 1] : ' ';
        if (after == '=') {
            ++i;
            continue;
        }
        if (before != '=' && before != '<' && before != '>' && before != '!') return true;
    }
    return false;
}

// The pieces between << in a printing line, said in words.
std::string print_words(const std::string& rest) {
    std::vector<std::string> pieces;
    std::string cur;
    bool quote = false;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const char ch = rest[i];
        if (ch == '"' && (i == 0 || rest[i - 1] != '\\')) quote = !quote;
        if (!quote && ch == '<' && i + 1 < rest.size() && rest[i + 1] == '<') {
            pieces.push_back(trim(cur));
            cur.clear();
            ++i;
            continue;
        }
        cur += ch;
    }
    pieces.push_back(trim(cur));
    std::vector<std::string> said;
    bool newline = false;
    for (std::string p : pieces) {
        if (!p.empty() && p.back() == ';') p = trim(p.substr(0, p.size() - 1));
        if (p.empty()) continue;
        if (p == "std::endl" || p == "endl" || p == "\"\\n\"" || p == "'\\n'") {
            newline = true;
            continue;
        }
        if (p.front() == '"') {
            std::string inner = p.substr(1, p.size() >= 2 ? p.size() - 2 : 0);
            if (inner.size() >= 2 && inner.compare(inner.size() - 2, 2, "\\n") == 0) {
                inner = inner.substr(0, inner.size() - 2);
                newline = true;
            }
            if (!inner.empty()) said.push_back("the words \"" + inner + "\"");
            continue;
        }
        static const std::regex name(R"(^[A-Za-z_]\w*$)");
        static const std::regex call(R"(^([A-Za-z_]\w*)\s*\(.*\)$)");
        static const std::regex size(R"(^([A-Za-z_]\w*)\.size\(\)$)");
        std::smatch mt;
        if (std::regex_match(p, name)) said.push_back("what's in the box " + p);
        else if (std::regex_match(p, mt, size)) said.push_back("how many things are in " + mt[1].str());
        else if (std::regex_match(p, mt, call)) said.push_back("the answer from the job " + mt[1].str());
        else said.push_back("the value of " + p);
    }
    std::string out = "This prints ";
    if (said.empty()) out = newline ? "This moves to a new line" : "This prints nothing";
    for (std::size_t i = 0; i < said.size(); ++i) out += (i == 0 ? "" : i + 1 == said.size() ? ", and then " : ", then ") + said[i];
    if (newline && !said.empty()) out += ", then moves to a new line";
    return out + ".";
}

// The line a { on `line` (1-based) opened, found from a } on `close`.
int block_start(const std::vector<std::string>& ls, int close) {
    int depth = 0;
    for (int i = close - 1; i >= 0; --i) {
        const std::string s = code_part(ls[static_cast<std::size_t>(i)]);
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (*it == '}') ++depth;
            else if (*it == '{' && --depth == 0) return i + 1;
        }
    }
    return 0;
}

std::string what_block(const std::string& opener) {
    const std::string s = code_part(opener);
    if (starts(s, "for") || starts(s, "while") || starts(s, "do")) return "the loop";
    if (starts(s, "if") || starts(s, "} else") || starts(s, "else")) return "the if";
    if (starts(s, "struct") || starts(s, "class")) return "the blueprint";
    if (s.find("main(") != std::string::npos) return "main";
    if (s.find('(') != std::string::npos) return "the job";
    return "the block";
}

std::string number_words(int n) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty"};
    if (n < 20) return ones[n];
    return std::string(tens[n / 10]) + (n % 10 ? std::string(" ") + ones[n % 10] : "");
}

int words_number(const std::vector<std::string>& w, std::size_t& i) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty"};
    if (i >= w.size()) return 0;
    if (std::isdigit(static_cast<unsigned char>(w[i][0]))) return std::atoi(w[i++].c_str());
    for (int t = 2; t <= 6; ++t)
        if (w[i] == tens[t]) {
            ++i;
            if (i < w.size())
                for (int o = 1; o <= 9; ++o)
                    if (w[i] == ones[o]) {
                        ++i;
                        return t * 10 + o;
                    }
            return t * 10;
        }
    for (int o = 0; o < 20; ++o)
        if (w[i] == ones[o]) {
            ++i;
            return o;
        }
    if (w[i] == "to" || w[i] == "too") return ++i, 2;   // "line to" is how "line two" is often heard
    if (w[i] == "for") return ++i, 4;
    return 0;
}

// The lines of a hint that are code (they start with four spaces).
std::vector<std::string> hint_code(const std::string& hint) {
    std::vector<std::string> out;
    for (const std::string& l : lines_of(hint))
        if (starts(l, "    ")) out.push_back(trim(l));
    return out;
}
std::string hint_words(const std::string& hint) {
    std::string out;
    for (const std::string& l : lines_of(hint))
        if (!starts(l, "    ") && !trim(l).empty()) out += (out.empty() ? "" : " ") + trim(l);
    return out;
}

// Where the learner's code goes: a // note saying "here", or "...".
int marker_line(const std::vector<std::string>& ls) {
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const std::string s = lower(ls[i]);
        const std::size_t at = s.find("//");
        if (at != std::string::npos && (s.find("here", at) != std::string::npos || s.find("...", at) != std::string::npos)) return static_cast<int>(i) + 1;
        if (s.find("/*") != std::string::npos && s.find("*/") != std::string::npos) return static_cast<int>(i) + 1;   // if (/* is name Max? */)
    }
    return 0;
}

std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') cur += c;
        else if (!cur.empty()) out.push_back(cur), cur.clear();
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The line most like `want` (by the words they share), 0 for none.
int most_like(const std::vector<std::string>& ls, const std::string& want) {
    const std::vector<std::string> w = words_of(want);
    int best = 0, best_score = 0;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const std::vector<std::string> have = words_of(code_part(ls[i]));
        int score = 0;
        for (const std::string& x : w) score += std::count(have.begin(), have.end(), x) > 0 ? 1 : 0;
        if (score > best_score) best_score = score, best = static_cast<int>(i) + 1;
    }
    return best_score >= 2 ? best : 0;
}

// The text in a line's "quotes" (all of them, joined), without \n.
std::string printed_text(const std::string& line) {
    std::string out;
    bool quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
            quote = !quote;
            continue;
        }
        if (!quote) continue;
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i;
            continue;
        }
        out += line[i];
    }
    return out;
}

// ---- the code words Bit can explain: how they're said, what they mean, and how to find them in code
struct Term {
    const char* word;                  // shown and looked up: "return 0"
    std::vector<const char*> said;     // how it's said out loud (and typed), longest first matters only within one word
    const char* means;
    const char* find;                  // a regex for the first line that uses it ("" for none)
};

const std::vector<Term>& terms() {
    static const std::vector<Term> all = {
        {"return 0", {"return zero", "return 0", "return nothing"},
         "return 0 at the end of main ends the program and tells the computer everything went fine. The 0 means \"no problems\".", R"(\breturn\s+0\s*;)"},
        {"return", {"return", "returns"}, "return hands an answer back from a job to whoever asked for it. The job stops right there.", R"(\breturn\b)"},
        {"cout", {"see out", "c out", "cout", "standard see out"}, "std::cout prints to the screen. Whatever comes after << gets printed, in order.", R"(\bcout\b)"},
        {"cin", {"see in", "c in", "cin"}, "std::cin reads what you type. >> points to the box it goes into.", R"(\bcin\b)"},
        {"<<", {"less than less than", "the two arrows", "two arrows", "the arrows", "double arrows"},
         "<< sends things to cout to be printed, one after another, like a conveyor belt to the screen.", R"(<<)"},
        {"\\n", {"backslash n", "back slash n", "slash n", "new line", "end line", "endl"},
         "\\n (or std::endl) moves to a new line, like pressing Enter.", R"(\\n|\bendl\b)"},
        {"int", {"int", "integer", "an int"}, "int is a box for whole numbers, like 3 or -12. No dots allowed.", R"(\bint\b)"},
        {"double", {"double", "a double"}, "double is a box for numbers with a decimal point, like 2.5.", R"(\bdouble\b)"},
        {"string", {"string", "a string", "strings"}, "std::string is a box for words, like a name. It needs #include <string>.", R"(\bstring\b)"},
        {"char", {"char", "character"}, "char is a box for one single letter, like 'a'.", R"(\bchar\b)"},
        {"bool", {"bool", "boolean", "true or false"}, "bool is a box that holds true or false, like the answer to a yes-or-no question.", R"(\bbool\b)"},
        {"vector", {"vector", "a vector", "vectors", "a list", "list"}, "std::vector is a list of boxes in a row. The first box is number 0.", R"(\bvector\b)"},
        {"#include", {"include", "hash include", "pound include", "hashtag include"},
         "#include brings in a toolbox of ready-made code, so your program can use it.", R"(#include)"},
        {"iostream", {"iostream", "i o stream", "eye o stream", "io stream"}, "iostream is the toolbox for printing to the screen and reading what you type.",
         R"(iostream)"},
        {"main", {"main", "int main", "the main function"}, "main is where every program starts. The computer runs the lines inside its { } from top to bottom.",
         R"(\bmain\s*\()"},
        {"std::", {"std", "s t d", "standard", "colon colon", "double colon"}, "std:: means \"from the standard toolbox\". std::cout is cout from that toolbox.",
         R"(std::)"},
        {";", {"semicolon", "semi colon", "the semicolon"}, "A semicolon ; ends a line that does something, like a full stop ends a sentence.", R"(;)"},
        {"{ }", {"curly brackets", "curly braces", "braces", "the curly brackets", "squiggly brackets"},
         "{ and } wrap a group of lines that belong together, like the inside of main, a loop or an if.", R"(\{)"},
        {"( )", {"parentheses", "round brackets", "brackets"}, "( ) hold the question of an if or a loop, or what you give to a job.", R"(\()"},
        {"[ ]", {"square brackets", "index"}, "[ ] picks one box from a list by its number: scores[0] is the first box.", R"(\[)"},
        {"==", {"double equals", "equals equals", "two equal signs", "two equals"}, "== asks \"are these the same?\" It doesn't change anything.", R"(==)"},
        {"=", {"equals", "equal sign", "one equal sign", "single equals", "the equal sign"}, "One = puts a value in a box. Whatever was in it before is gone.",
         R"([^=!<>]=[^=])"},
        {"+=", {"plus equals"}, "x += 5 adds 5 to what's already in x. It's short for x = x + 5.", R"(\+=)"},
        {"++", {"plus plus"}, "i++ adds 1 to i.", R"(\+\+)"},
        {"<", {"less than", "smaller than", "the less than sign"}, "< asks \"is the left one smaller?\" i < 5 is true for 0 to 4, but not 5.", R"([^<]<[^<=])"},
        {"<=", {"less than or equal", "less than or equal to", "less than equals"}, "<= asks \"is the left one smaller, or the same?\" i <= 5 lets 5 have a turn too.",
         R"(<=)"},
        {"for", {"for loop", "a for loop", "for"}, "A for loop runs its lines again and again: it starts a counter, checks a question each time, and moves the counter on.",
         R"(\bfor\s*\()"},
        {"while", {"while loop", "a while loop", "while"}, "A while loop keeps running its lines as long as its question is yes.", R"(\bwhile\s*\()"},
        {"if", {"if", "an if", "if statement"}, "if asks a question. When the answer is yes, the lines in its { } run; when it's no, they're skipped.", R"(\bif\s*\()"},
        {"else", {"else"}, "else is what happens when the if's question was no.", R"(\belse\b)"},
        {"void", {"void"}, "void means a job hands nothing back. It just does its work.", R"(\bvoid\b)"},
        {"const", {"const", "constant"}, "const means a box can never change after it gets its first value.", R"(\bconst\b)"},
        {"&", {"ampersand", "and sign", "the and sign", "reference"}, "& after a type, like int& n, means the job gets the real box, not a copy.", R"(&[^&])"},
        {"&&", {"and and", "double and", "two and signs"}, "&& means \"and\": both questions must be yes.", R"(&&)"},
        {"function", {"function", "a function", "functions", "job", "a job"},
         "A function (a job) is a set of lines with a name. You write it once and use it as many times as you like.", ""},
        {"variable", {"variable", "a variable", "variables", "box", "a box"}, "A variable is a named box that holds a value. Its type says what can go inside.", ""},
        {"struct", {"struct", "a struct", "blueprint", "a blueprint", "class"}, "A struct is a blueprint: it says what every thing of that kind has inside it.",
         R"(\bstruct\b|\bclass\b)"},
        {"//", {"comment", "a comment", "slash slash", "the green text", "notes"}, "// starts a note for people. The computer skips everything after it on that line.",
         R"(//)"},
        {"push_back", {"push back"}, "push_back adds a new box to the end of a list.", R"(push_back)"},
        {"size", {"size", "dot size"}, ".size() tells you how many boxes are in a list, or letters in a word.", R"(\.size\(\))"},
        {"break", {"break"}, "break jumps straight out of a loop.", R"(\bbreak\s*;)"},
        {"continue", {"continue"}, "continue skips the rest of this time round a loop and goes on to the next.", R"(\bcontinue\s*;)"},
        {"printf", {"print f", "printf"}, "printf is C's way to print. Each % spot gets filled in with a value after the comma.", R"(\bprintf\b)"},
    };
    return all;
}

const char* const kAskForms[] = {"what does {} mean", "what does {} do", "what is {}", "what's {}", "what are {}", "explain {}", "tell me about {}",
                                 "what is a {}", "what's a {}", "what does the {} mean", "what does the {} do", "what is the {}", "explain the {}"};

bool prints(const std::string& s) { return s.find("cout") != std::string::npos || s.find("printf") != std::string::npos || s.find("puts(") != std::string::npos; }

}  // namespace

int block_end(const std::string& code, int line) {
    const std::vector<std::string> ls = lines_of(code);
    if (line < 1 || line > static_cast<int>(ls.size())) return line;
    int depth = 0;
    bool opened = false;
    for (std::size_t i = static_cast<std::size_t>(line - 1); i < ls.size(); ++i) {
        for (char c : code_part(ls[i])) {
            if (c == '{') ++depth, opened = true;
            else if (c == '}' && --depth == 0 && opened) return static_cast<int>(i) + 1;
        }
        if (!opened && i > static_cast<std::size_t>(line - 1)) break;   // the line opens no block
    }
    return line;
}

int first_block_line(const std::string& code) {
    const std::vector<std::string> ls = lines_of(code);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const std::string s = code_part(ls[i]);
        if ((starts(s, "for") || starts(s, "if") || starts(s, "while")) && !s.empty() && s.back() == '{') return static_cast<int>(i) + 1;
    }
    return 0;
}

std::string explain_line(const std::string& code, int line) {
    const std::vector<std::string> ls = lines_of(code);
    if (line < 1 || line > static_cast<int>(ls.size())) return "There's no line " + std::to_string(line) + ". This code has " + std::to_string(ls.size()) + " lines.";
    const std::string raw = trim(ls[static_cast<std::size_t>(line - 1)]);
    const std::string s = code_part(raw);
    std::smatch m;

    if (raw.empty()) return "This line is empty. It just gives the code some room to breathe.";
    if (s.empty()) {   // only a note
        const std::string l = lower(raw);
        if (l.find("here") != std::string::npos || l.find("...") != std::string::npos)
            return "This is a note that marks where your code goes. Swap it for your own line!";
        return "This is a note for people reading the code. The computer skips it.";
    }
    if (starts(s, "/*") && s.find("*/") != std::string::npos && s.size() == s.find("*/") + 2)
        return "This is a note for people reading the code. The computer skips it.";
    if (starts(s, "#include")) {
        if (s.find("iostream") != std::string::npos) return "This brings in iostream: the toolbox for printing to the screen and reading what you type.";
        if (s.find("vector") != std::string::npos) return "This brings in vector: the toolbox for lists.";
        if (s.find("string") != std::string::npos && s.find("string.h") == std::string::npos) return "This brings in string: the toolbox for words.";
        if (s.find("stdio") != std::string::npos) return "This brings in stdio: C's toolbox for printing and reading.";
        return "This brings in a toolbox of ready-made code, so the program can use it.";
    }
    if (starts(s, "using namespace std")) return "This lets you write cout instead of std::cout.";
    if (s == "{") return "This { opens a group of lines that belong together.";
    if (s == "}" || s == "};" || starts(s, "} else")) {
        const int open = block_start(ls, line);
        std::string out = open ? "This } closes " + what_block(ls[static_cast<std::size_t>(open - 1)]) + " that started on line " + std::to_string(open) + "."
                               : "This } closes a group of lines.";
        if (starts(s, "} else if")) out += " Then, if that question was no, it asks another one.";
        else if (starts(s, "} else")) out += " Then the else part starts: it runs when the question was no.";
        return out;
    }
    if (std::regex_search(s, std::regex(R"(\bmain\s*\()"))) return "This is main: where the program starts. The computer runs the lines inside its { } from top to bottom.";
    if (std::regex_match(s, m, std::regex(R"(return\s*(\d+)\s*;)")) && m[1] == "0") {
        for (int i = line - 1; i >= 1; --i)
            if (std::regex_search(ls[static_cast<std::size_t>(i - 1)], std::regex(R"(\bmain\s*\()")))
                return "This ends the program and tells the computer everything went fine.";
    }
    if (std::regex_match(s, m, std::regex(R"(return\s*;)"))) return "This ends the job right here, without handing anything back.";
    if (std::regex_match(s, m, std::regex(R"(return\s+(.+);)"))) return "This job is done: it hands back " + condition_words(m[1].str()) + " to whoever asked.";
    if (std::regex_match(s, m, std::regex(R"((struct|class)\s+(\w+).*)"))) return "This is a blueprint called " + m[2].str() + ". It says what every " + m[2].str() + " has inside it.";
    if (std::regex_match(s, m, std::regex(R"(for\s*\(\s*(?:const\s+)?[\w:<>]+\s*&?\s*(\w+)\s*:\s*(\w+)\s*\)\s*\{?)")))
        return "This goes through the list " + m[2].str() + ", one box at a time. Each time round, " + m[1].str() + " holds the next one.";
    if (std::regex_match(s, m, std::regex(R"(for\s*\((.*);(.*);(.*)\)\s*\{?)"))) {
        std::string init = trim(m[1].str()), cond = trim(m[2].str()), step = trim(m[3].str());
        std::smatch im;
        std::string start = "with " + init;
        if (std::regex_match(init, im, std::regex(R"((?:[\w:]+\s+)?(\w+)\s*=\s*(.+))"))) start = "with " + im[1].str() + " at " + im[2].str();
        std::string each = "it does " + step;
        std::smatch sm;
        if (std::regex_match(step, sm, std::regex(R"((\w+)\s*(\+\+|--))")) || std::regex_match(step, sm, std::regex(R"((\+\+|--)\s*(\w+))"))) {
            const bool up = step.find("++") != std::string::npos;
            const std::string v = sm[1].str() == "++" || sm[1].str() == "--" ? sm[2].str() : sm[1].str();
            each = v + (up ? " goes up by 1" : " goes down by 1");
        } else if (std::regex_match(step, sm, std::regex(R"((\w+)\s*=\s*(\w+)\s*([+-])\s*(\d+))")) && sm[1] == sm[2]) {
            each = sm[1].str() + (sm[3] == "+" ? " goes up by " : " goes down by ") + sm[4].str();
        } else if (std::regex_match(step, sm, std::regex(R"((\w+)\s*([+-])=\s*(\d+))"))) {
            each = sm[1].str() + (sm[2] == "+" ? " goes up by " : " goes down by ") + sm[3].str();
        }
        return "This is a loop. It starts " + start + ", and keeps going while " + condition_words(cond) + ". Each time round, " + each + ".";
    }
    if (std::regex_match(s, m, std::regex(R"(while\s*\((.*)\)\s*\{?)")))
        return "This loop keeps running the lines inside it, again and again, as long as " + condition_words(m[1].str()) + ".";
    if (std::regex_match(s, m, std::regex(R"(do\s*\{?)"))) return "This loop runs its lines once, then keeps going while the question at its end is yes.";
    if (std::regex_match(s, m, std::regex(R"((\}\s*)?else\s+if\s*\((.*)\)\s*\{?)"))) {
        std::string out = "If the question before was no, this asks another one: is it true that " + condition_words(m[2].str()) + "?";
        if (lone_equals(m[2].str())) out += " Careful: one = puts a value in a box. To ask \"is it the same?\", use ==.";
        return out;
    }
    if (std::regex_match(s, m, std::regex(R"((\}\s*)?else\s*\{?)"))) return "If none of the questions above were yes, the lines in this else part run instead.";
    if (std::regex_match(s, m, std::regex(R"(if\s*\((.*)\)\s*\{?)"))) {
        std::string out = "This asks a question: is it true that " + condition_words(m[1].str()) + "? If yes, the lines inside its { } run. If not, they're skipped.";
        if (lone_equals(m[1].str())) out += " Careful: one = puts a value in a box. To ask \"is it the same?\", use ==.";
        return out;
    }
    if (std::regex_search(s, m, std::regex(R"((?:std::)?cout\s*<<(.*))"))) return print_words(m[1].str());
    if (std::regex_search(s, m, std::regex(R"(printf\s*\(\s*"((?:[^"\\]|\\.)*)\")"))) {
        std::string w = m[1].str();
        const bool nl = w.size() >= 2 && w.compare(w.size() - 2, 2, "\\n") == 0;
        if (nl) w = w.substr(0, w.size() - 2);
        std::string out = "This prints the words \"" + w + "\"";
        if (w.find('%') != std::string::npos) out += ". Each % spot gets filled in with the values after the comma";
        return out + (nl ? ", then moves to a new line." : ".");
    }
    if (std::regex_search(s, m, std::regex(R"((?:std::)?cin\s*>>\s*(.*);)"))) {
        std::string boxes = std::regex_replace(m[1].str(), std::regex(R"(\s*>>\s*)"), " and ");
        return "This waits for you to type something, then puts what you typed into " + boxes + ".";
    }
    if (std::regex_search(s, m, std::regex(R"(getline\s*\(\s*(?:std::)?cin\s*,\s*(\w+))")))
        return "This waits for you to type a whole line, then puts it in " + m[1].str() + ".";
    if (std::regex_search(s, m, std::regex(R"(cin\.ignore)"))) return "This throws away the Enter key left over from the last thing you typed.";
    if (std::regex_match(s, m, std::regex(R"((\w+)\.push_back\((.*)\);)"))) return "This adds " + m[2].str() + " to the end of the list " + m[1].str() + ".";
    // a job being written: "int add(int a, int b) {"
    if (std::regex_match(s, m, std::regex(R"(((?:const\s+)?[\w:<>]+)\s*[&*]?\s+(\w+)\s*\((.*)\)\s*(const\s*)?\{?)")) && m[1] != "return" &&
        m[1] != "else") {
        const std::string ret = m[1].str(), name = m[2].str(), params = trim(m[3].str());
        std::string out = "This starts a job called " + name + ".";
        if (!params.empty() && params != "void") {
            std::string needs;
            std::stringstream ps(params);
            std::string p;
            int n = 0;
            while (std::getline(ps, p, ',')) {
                std::smatch pm;
                const std::string t = trim(p);
                if (std::regex_match(t, pm, std::regex(R"(((?:const\s+)?[\w:<>]+)\s*(&?)\s*(\w+))"))) {
                    needs += (n++ ? ", and " : "") + pm[3].str() + " (" + type_words(pm[1].str()) + (pm[2] == "&" ? ", the real box, not a copy" : "") + ")";
                }
            }
            if (!needs.empty()) out += " It needs " + needs + ".";
        }
        out += ret == "void" ? " It doesn't hand anything back." : " It hands back " + type_words(ret) + ".";
        return out;
    }
    // a box being made: "int total = 0;", "double each;", "std::vector<int> scores{3, 7, 5};"
    if (std::regex_match(s, m, std::regex(R"(((?:const\s+)?(?:unsigned\s+|long\s+)?[\w:]+(?:<[^>]*>)?)\s*(&?)\s+(\w+)\s*(?:=\s*(.+)|\{(.*)\})?\s*;)")) &&
        m[1] != "return" && m[1] != "delete") {
        const std::string type = m[1].str(), name = m[3].str();
        std::string out = "This makes a box called " + name + " that holds " + type_words(type);
        if (m[2] == "&") return out + ". The & means it's the real box from somewhere else, not a copy.";
        if (m[4].matched) return out + ", and puts " + trim(m[4].str()) + " in it.";
        if (m[5].matched) return out + ", and fills it with " + trim(m[5].str()) + ".";
        return out + ", but puts nothing in it yet. Until it gets a value, it holds junk!";
    }
    if (std::regex_match(s, m, std::regex(R"(([\w.\[\]]+)\s*(\+\+|--)\s*;)")) || std::regex_match(s, m, std::regex(R"((\+\+|--)\s*([\w.\[\]]+)\s*;)"))) {
        const bool up = s.find("++") != std::string::npos;
        const std::string v = m[1].str() == "++" || m[1].str() == "--" ? m[2].str() : m[1].str();
        return std::string("This ") + (up ? "adds 1 to " : "takes 1 away from ") + v + ".";
    }
    if (std::regex_match(s, m, std::regex(R"(([\w.\[\]]+)\s*([+\-*/])=\s*(.+);)"))) {
        const std::string v = m[1].str(), op = m[2].str(), e = m[3].str();
        if (op == "+") return "This adds " + e + " to what's already in " + v + ".";
        if (op == "-") return "This takes " + e + " away from what's in " + v + ".";
        if (op == "*") return "This multiplies what's in " + v + " by " + e + ".";
        return "This divides what's in " + v + " by " + e + ".";
    }
    if (std::regex_match(s, m, std::regex(R"(([\w.\[\]]+)\s*=\s*([^=].*);)")))
        return "This puts a new value in " + m[1].str() + ": " + trim(m[2].str()) + ". Whatever was in " + m[1].str() + " before is gone.";
    if (std::regex_match(s, m, std::regex(R"(break\s*;)"))) return "This jumps out of the loop straight away.";
    if (std::regex_match(s, m, std::regex(R"(continue\s*;)"))) return "This skips the rest of this time round the loop, and goes on to the next.";
    if (std::regex_match(s, m, std::regex(R"((\w+)\s*\((.*)\)\s*;)"))) {
        std::string out = "This asks the job " + m[1].str() + " to do its work";
        if (!trim(m[2].str()).empty()) out += ", with " + trim(m[2].str());
        out += ".";
        for (std::size_t i = 0; i < ls.size(); ++i)
            if (static_cast<int>(i) + 1 != line && std::regex_search(ls[i], std::regex("\\b" + m[1].str() + R"(\s*\([^;]*\)\s*\{?\s*$)")))
                return out + " That job is written on line " + std::to_string(i + 1) + ".";
        return out;
    }
    return "I'm not sure how to say this one in plain words yet. Try changing it a little and pressing Run to see what it does!";
}

TeacherSay explain_at(const std::string& code, int line) {
    TeacherSay t;
    t.text = "Line " + std::to_string(line) + ". " + explain_line(code, line);
    t.line = line;
    t.line_to = line;
    const std::vector<std::string> ls = lines_of(code);
    if (line >= 1 && line <= static_cast<int>(ls.size())) {
        const std::string s = code_part(ls[static_cast<std::size_t>(line - 1)]);
        if (!s.empty() && s.back() == '{' && !std::regex_search(s, std::regex(R"(\bmain\s*\()"))) t.line_to = block_end(code, line);
    }
    return t;
}

TeacherSay hint_help(const Lesson& l, const std::string& code) {
    TeacherSay t;
    const std::vector<std::string> ls = lines_of(code);
    const std::vector<std::string> wanted = hint_code(l.hint);
    t.line = marker_line(ls);
    if (!t.line && !wanted.empty()) t.line = most_like(ls, wanted.front());
    t.line_to = t.line;
    std::string words = hint_words(l.hint);
    if (words.empty()) words = "Here's a hint.";
    if (words.back() == ':') words.back() = '.';
    t.text = words;
    if (t.line) t.text += " Look at line " + std::to_string(t.line) + ".";
    if (!wanted.empty()) t.code = wanted.front();
    return t;
}

TeacherSay why_help(const Lesson& l, const LessonCheck& c, bool ran, const std::string& code) {
    TeacherSay t;
    const std::vector<std::string> ls = lines_of(code);
    if (!ran) {
        t.text = "Press Run first, then I can tell you what happened!";
        return t;
    }
    if (c.passed) {
        t.text = "It works! It prints exactly what it should. Great job!";
        return t;
    }
    if (!c.run.compiled) {
        std::smatch m;
        const std::string& e = c.run.compile_errors;
        if (std::regex_search(e, m, std::regex(R"(\.(?:c|cpp|cc):(\d+):\d+: (?:fatal )?error)"))) t.line = std::stoi(m[1].str());
        t.line_to = t.line;
        t.text = (t.line ? "The computer got stuck reading line " + std::to_string(t.line) + ". " : "The computer couldn't build it. ") + plain_error(e);
        if (t.line >= 1 && t.line <= static_cast<int>(ls.size())) {
            // a missing ; is reported where the next line starts: point at the line that needs it
            if (e.find("expected ';'") != std::string::npos) {
                int prev = t.line;
                while (prev > 1 && trim(ls[static_cast<std::size_t>(prev - 1)]).empty()) --prev;
                const std::string here = trim(ls[static_cast<std::size_t>(prev - 1)]);
                if (prev > 1 && (here.empty() || here.back() != ';')) {
                    const std::string before = trim(ls[static_cast<std::size_t>(prev - 2)]);
                    if (!before.empty() && before.back() != ';' && before.back() != '{' && before.back() != '}') t.line = t.line_to = prev - 1;
                }
                t.text = "Line " + std::to_string(t.line) + " is missing a semicolon ; at its end. Every line that does something ends with one.";
            }
        }
        return t;
    }
    if (c.run.timed_out || c.run.crashed) {
        for (std::size_t i = 0; i < ls.size(); ++i) {
            const std::string s = code_part(ls[i]);
            if (starts(s, "for") || starts(s, "while")) {
                t.line = static_cast<int>(i) + 1;
                break;
            }
        }
        t.line_to = t.line ? block_end(code, t.line) : 0;
        t.text = c.run.timed_out ? "It got stuck going round and round. Is there a loop that never ends?"
                                 : "It crashed. That often means reading a box past the end of a list.";
        if (t.line) t.text += " Look at the loop on line " + std::to_string(t.line) + ".";
        return t;
    }
    if (l.expected.empty()) {
        t.line = t.line_to = marker_line(ls);
        t.text = "It runs, but it's still the code you started with. Do what YOUR TASK says.";
        if (t.line) t.text += " Your code goes on line " + std::to_string(t.line) + ".";
        return t;
    }
    // it ran and printed something else: find the first line that's different
    const std::vector<std::string> want = lines_of(normalize_output(l.expected));
    const std::vector<std::string> got = lines_of(normalize_output(c.run.output));
    std::size_t k = 0;
    while (k < want.size() && k < got.size() && trim(want[k]) == trim(got[k])) ++k;
    const std::string w = k < want.size() ? trim(want[k]) : "";
    const std::string g = k < got.size() ? trim(got[k]) : "";
    // the printing line whose words match the most of the line it should print
    int best = 0;
    std::size_t best_len = 0;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        if (!prints(ls[i])) continue;
        const std::string said = trim(printed_text(ls[i]));
        std::size_t n = 0;
        const std::string& target = w.empty() ? g : w;
        while (n < said.size() && n < target.size() && said[n] == target[n]) ++n;
        if (n > best_len || (n == best_len && n > 0 && !best)) best_len = n, best = static_cast<int>(i) + 1;
    }
    if (!best_len) best = marker_line(ls);
    t.line = t.line_to = best;
    // both lines are in the code: the wrong one ran, so a question chose the wrong way
    if (best_len && !w.empty() && !g.empty()) {
        int printed_g = 0;
        for (std::size_t i = 0; i < ls.size() && !printed_g; ++i)
            if (prints(ls[i]) && !trim(printed_text(ls[i])).empty() && g.rfind(trim(printed_text(ls[i])), 0) == 0 && static_cast<int>(i) + 1 != best)
                printed_g = static_cast<int>(i) + 1;
        if (printed_g) {
            for (int i = std::min(best, printed_g) - 1; i >= 1; --i) {
                const std::string s = code_part(ls[static_cast<std::size_t>(i - 1)]);
                if (starts(s, "if") || starts(s, "} else if") || starts(s, "else if")) {
                    t.line = t.line_to = i;
                    t.text = "So close! It printed \"" + g + "\", but it should print \"" + w + "\". The question on line " + std::to_string(i) +
                             " chose the wrong way. Look at it closely!";
                    return t;
                }
            }
        }
    }
    if (k >= got.size() && !w.empty())
        t.text = "So close! It should also print \"" + w + "\", but it stopped before that.";
    else if (w.empty())
        t.text = "So close! It printed an extra line: \"" + g + "\".";
    else
        t.text = "So close! It printed \"" + g + "\", but it should print \"" + w + "\".";
    if (t.line) t.text += " Look at line " + std::to_string(t.line) + ".";
    return t;
}

TeacherSay intro(const Lesson& l, const std::string& name) {
    TeacherSay t;
    t.text = (name.empty() ? "Hi, I'm Bit! " : "Hi " + name + ", I'm Bit! ") + "Let's do this one together. " + l.task;
    return t;
}

std::string clean_name(const std::string& typed) {
    std::string s = " " + lower(trim(typed)) + " ";
    for (const char* lead : {" my name is ", " my name's ", " i am ", " i'm ", " im ", " call me ", " it's ", " its ", " this is ", " hi ", " hello "})
        if (s.rfind(lead, 0) == 0) s = " " + s.substr(std::string(lead).size());
    // the first word, from what was typed (so "MCKENZIE" or "mckenzie" can be tidied but "McKenzie" is kept)
    std::string original = trim(typed);
    const std::string low = lower(original);
    const std::string rest = trim(s);
    const std::size_t at = rest.empty() ? std::string::npos : low.find(rest.substr(0, rest.find(' ')));
    std::string word;
    if (at != std::string::npos)
        for (std::size_t i = at; i < original.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(original[i]);
            if (std::isalpha(c) || c >= 0x80 || ((c == '-' || c == '\'') && !word.empty())) word += original[i];
            else if (!word.empty()) break;
        }
    while (!word.empty() && (word.back() == '-' || word.back() == '\'')) word.pop_back();
    if (word.size() > 20) word.resize(20);
    if (word.empty()) return word;
    bool has_upper = false, has_lower = false;
    for (char c : word) {
        if (std::isupper(static_cast<unsigned char>(c))) has_upper = true;
        if (std::islower(static_cast<unsigned char>(c))) has_lower = true;
    }
    if (!(has_upper && has_lower)) {   // all one case: make it Sam
        word = lower(word);
        bool start = true;
        for (char& c : word) {
            if (start && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            start = c == '-' || c == '\'';
        }
    }
    return word;
}

std::string read_aloud(const Lesson& l) {
    std::string out = l.title + ". ";
    for (const std::string& p : l.instructions) {
        std::string s = starts(p, "- ") ? p.substr(2) : p;
        if (!s.empty() && s.back() != '.' && s.back() != '!' && s.back() != '?' && s.back() != ':') s += '.';
        out += s + " ";
    }
    return out + "Your task: " + l.task;
}

Heard understand(const std::string& said) {
    Heard h;
    std::string s = lower(said);
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '\'') c = ' ';
    s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
    std::vector<std::string> w;
    {
        std::stringstream ss(s);
        std::string x;
        while (ss >> x) w.push_back(x);
    }
    auto has = [&](const char* word) { return std::find(w.begin(), w.end(), word) != w.end(); };
    auto phrase = [&](const char* p) { return (" " + s + " ").find(" " + std::string(p) + " ") != std::string::npos; };
    for (std::size_t i = 0; i + 1 < w.size(); ++i)
        if (w[i] == "line" || w[i] == "lines") {
            std::size_t j = i + 1;
            if (w[j] == "number") ++j;
            const int n = words_number(w, j);
            if (n > 0) {
                h.ask = Ask::Line;
                h.line = n;
                return h;
            }
        }
    if (phrase("this line") || phrase("that line") || phrase("my line")) return {Ask::ThisLine, 0};
    // "what does return 0 mean?": a question about a code word (the longest word heard wins: "return 0" over "return")
    if (phrase("mean") || phrase("means") || s.find("what is ") != std::string::npos || s.find("whats ") != std::string::npos ||
        s.find("what are ") != std::string::npos || s.find("what does ") != std::string::npos || s.find("explain ") != std::string::npos ||
        s.find("tell me about ") != std::string::npos) {
        const Term* best = nullptr;
        std::size_t best_len = 0;
        for (const Term& term : terms())
            for (const char* form : term.said) {
                std::string f = lower(form);
                f.erase(std::remove(f.begin(), f.end(), '\''), f.end());
                if (f.size() > best_len && phrase(f.c_str())) best = &term, best_len = f.size();
            }
        if (best) {
            Heard w;
            w.ask = Ask::Word;
            w.word = best->word;
            return w;
        }
    }
    if (phrase("didnt it work") || phrase("didnt work") || phrase("whats wrong") || phrase("what is wrong") || phrase("went wrong") ||
        phrase("is it wrong") || phrase("did it fail") || phrase("not working") || phrase("doesnt work") || phrase("why"))
        return {Ask::Why, 0};
    if (has("hint") || has("help") || has("stuck") || phrase("what do i write")) return {Ask::Hint, 0};
    if (has("read") || phrase("what do i do") || phrase("the task") || phrase("whats the task")) return {Ask::Read, 0};
    if (has("run")) return {Ask::Run, 0};
    if (has("thanks") || phrase("thank you")) return {Ask::Thanks, 0};
    if (has("bye") || has("goodbye") || phrase("go away")) return {Ask::Bye, 0};
    if (has("hello") || has("hi") || has("hey") || has("wave")) return {Ask::Hello, 0};
    return h;
}

TeacherSay word_help(const std::string& word, const std::string& code) {
    TeacherSay t;
    const auto it = std::find_if(terms().begin(), terms().end(), [&](const Term& x) { return word == x.word; });
    if (it == terms().end()) {
        t.text = "Which part? Try asking \"what does return 0 mean?\" or \"what is cout?\"";
        return t;
    }
    t.text = it->means;
    if (*it->find) {
        const std::regex re(it->find);
        const std::vector<std::string> ls = lines_of(code);
        for (std::size_t i = 0; i < ls.size(); ++i) {
            const std::string s = word == "//" ? ls[i] : code_part(ls[i]);
            if (std::regex_search(" " + s + " ", re)) {
                t.line = t.line_to = static_cast<int>(i) + 1;
                t.text += " Look, it's on line " + std::to_string(i + 1) + ".";
                break;
            }
        }
    }
    return t;
}

std::vector<std::string> spoken_phrases() {
    std::vector<std::string> out = {
        "what does this line do", "explain this line", "this line",
        "give me a hint", "hint", "help", "help me", "i need help", "i'm stuck", "i am stuck", "show me a hint", "what do i write",
        "why didn't it work", "what's wrong", "what is wrong", "what went wrong", "why is it wrong", "why did it fail", "it's not working",
        "read it to me", "read the lesson", "read it", "read the task", "what do i do", "what is the task",
        "run it", "run my code", "run",
        "hello", "hi", "hey bit", "hi bit", "hello bit", "wave",
        "bye", "goodbye", "go away", "bye bit", "thank you", "thanks", "thank you bit",
    };
    for (const Term& term : terms())
        for (const char* form : term.said) {
            const std::string f = form;
            if (std::any_of(f.begin(), f.end(), [](char c) { return !std::isalpha(static_cast<unsigned char>(c)) && c != ' '; })) continue;   // words only
            const bool article = starts(f, "a ") || starts(f, "an ") || starts(f, "the ");
            for (const char* ask : kAskForms) {
                std::string p = ask;
                if (article && (p.find("a {}") != std::string::npos || p.find("the {}") != std::string::npos)) continue;
                p.replace(p.find("{}"), 2, f);
                out.push_back(p);
            }
        }
    for (int n = 1; n <= 60; ++n) {
        const std::string num = number_words(n);
        for (const char* form : {"what does line ", "explain line ", "line ", "what is line ", "help with line ", "show me line ", "what's on line "})
            out.push_back(form + num + (std::string(form) == "what does line " ? " do" : ""));
    }
    return out;
}

std::string spoken(const std::string& text) {
    std::string s = text;
    static const std::pair<std::regex, const char*> said[] = {
        {std::regex(R"(std::)"), ""},
        {std::regex(R"(\bcout\b)"), "see-out"},
        {std::regex(R"(\bcin\b)"), "see-in"},
        {std::regex(R"(\bendl\b)"), "end-line"},
        {std::regex(R"(\bprintf\b)"), "print-f"},
        {std::regex(R"(\\n)"), " new line "},
        {std::regex(R"(\+\+)"), " plus plus "},
        {std::regex(R"(==)"), " double equals "},
        {std::regex(R"(!=)"), " not equals "},
        {std::regex(R"(<=)"), " less than or equal to "},
        {std::regex(R"(>=)"), " greater than or equal to "},
        {std::regex(R"(<<)"), " "},
        {std::regex(R"(&&)"), " and "},
        {std::regex(R"(\|\|)"), " or "},
        {std::regex(R"(\{ \})"), "curly brackets"},
        {std::regex(R"(\b(\w+)\(\))"), "$1"},
        {std::regex(R"(\s;\s|\s;$)"), " semicolon "},
        {std::regex(R"(\be\.g\.,?)"), "for example,"},
        {std::regex(R"(\s&\s)"), " and "},
        {std::regex(R"([*_#`~|\[\]{}]+)"), " "},
    };
    for (const auto& [re, words] : said) s = std::regex_replace(s, re, words);
    s = std::regex_replace(s, std::regex(R"(\s+)"), " ");
    s = std::regex_replace(s, std::regex(R"(\s+([,.!?:]))"), "$1");
    return trim(s);
}

}  // namespace sparring
