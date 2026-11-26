#include "ast.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "compile.hpp"
#include "json.hpp"

namespace sparring {

namespace {

std::string base_lower(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    std::string b = pos == std::string::npos ? path : path.substr(pos + 1);
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return b;
}

// Splits the dump's TranslationUnitDecl into its top-level declarations as the text streams by, keeping only those whose location is
// in the user's file. Clang writes a location's "file" only when it changes, so the current file is carried from node to node.
class Scanner {
public:
    explicit Scanner(std::string user_base) : user_base_(std::move(user_base)) {}

    void feed(const char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) step(p[i]);
    }

    std::vector<std::string> decls;

private:
    struct Frame {
        char type;
        bool included_from;
    };

    void step(char c) {
        if (keep_ || (capturing_ && !decided_)) buf_ += c;
        if (in_str_) {
            if (esc_) {
                esc_ = false;
                if (str_.size() < 4096) str_ += c;
            } else if (c == '\\') {
                esc_ = true;
            } else if (c == '"') {
                in_str_ = false;
                on_string();
            } else if (str_.size() < 4096) {
                str_ += c;
            }
            return;
        }
        switch (c) {
        case '"':
            in_str_ = true;
            str_.clear();
            break;
        case ':':
            key_ = str_;
            after_colon_ = true;
            if (capturing_ && !decided_ && stack_.size() == 3 && key_ != "id" && key_ != "kind" && key_ != "loc") decide();
            break;
        case ',':
            after_colon_ = false;
            break;
        case '{':
        case '[': {
            const bool start = c == '{' && !capturing_ && stack_.size() == 2 && stack_[0].type == '{' && stack_[1].type == '[';
            stack_.push_back({c, after_colon_ && key_ == "includedFrom"});
            after_colon_ = false;
            if (start) {
                capturing_ = true;
                decided_ = false;
                keep_ = false;
                decl_file_.clear();
                buf_.assign(1, '{');
            }
            break;
        }
        case '}':
        case ']':
            if (!stack_.empty()) stack_.pop_back();
            after_colon_ = false;
            if (capturing_ && stack_.size() == 2) {
                if (!decided_) decide();
                if (keep_) decls.push_back(std::move(buf_));
                buf_.clear();
                capturing_ = false;
                keep_ = false;
            }
            break;
        default:
            break;
        }
    }

    void on_string() {
        if (after_colon_ && key_ == "file" && !(stack_.empty() || stack_.back().included_from)) {
            cur_file_ = str_;
            if (capturing_ && !decided_) decl_file_ = str_;
        }
        after_colon_ = false;
    }

    void decide() {
        decided_ = true;
        keep_ = base_lower(decl_file_.empty() ? cur_file_ : decl_file_) == user_base_;
        if (!keep_) {
            buf_.clear();
            buf_.shrink_to_fit();
        }
    }

    std::string user_base_, cur_file_, decl_file_, buf_, str_, key_;
    std::vector<Frame> stack_;
    bool in_str_ = false, esc_ = false, after_colon_ = false;
    bool capturing_ = false, decided_ = false, keep_ = false;
};

using json::JParser;
using json::JType;
using json::JValue;

bool is_decl(const std::string& k) {
    return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl" || k == "CXXRecordDecl" || k == "VarDecl" ||
           k == "ParmVarDecl" || k == "FieldDecl";
}

bool is_kept(const std::string& k) {
    return is_decl(k) || k == "ReturnStmt" || k == "CallExpr" || k == "DeclRefExpr" || k == "MemberExpr" || k == "BinaryOperator" ||
           k == "CompoundAssignOperator" || k == "ForStmt" || k == "WhileStmt" || k == "IfStmt";
}

bool is_scope(const std::string& k) {
    return k == "CompoundStmt" || k == "ForStmt" || k == "CXXForRangeStmt" || k == "WhileStmt" || k == "DoStmt" || k == "IfStmt" ||
           k == "SwitchStmt";
}

bool is_function(const std::string& k) { return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl"; }

// Walks a declaration in the order Clang wrote it, following the line numbers it only writes when they change.
struct Walker {
    std::vector<AstNode>& out;
    int cur_line = 0;
    std::vector<int> scopes;              // the last lines of the blocks around the node
    std::vector<std::string> functions;   // the jobs around the node

    void loc(const JValue& l, int* line_out) {
        if (const JValue* sp = l.find("spellingLoc")) {
            loc(*sp, nullptr);
            if (const JValue* ex = l.find("expansionLoc")) loc(*ex, line_out);
            return;
        }
        if (const JValue* ln = l.find("line")) cur_line = static_cast<int>(ln->num);
        if (line_out) *line_out = cur_line;
    }

    void node(const JValue& n) {
        if (n.type != JType::Object) return;
        int line = -1;
        int end = -1;
        if (const JValue* l = n.find("loc")) loc(*l, &line);
        if (const JValue* r = n.find("range")) {
            if (const JValue* b = r->find("begin")) loc(*b, line < 0 ? &line : nullptr);
            if (const JValue* e = r->find("end")) loc(*e, &end);
        }
        if (line < 0) line = cur_line;
        if (end < line) end = line;
        const JValue* implicit = n.find("isImplicit");
        if (implicit && implicit->boolean) return;   // the compiler's own (copy constructors and the like): nothing the reader wrote
        const std::string kind = n.text("kind");
        if (is_kept(kind)) {
            AstNode a;
            a.kind = kind;
            a.line = line;
            a.end_line = end;
            a.scope_end = scopes.empty() ? (1 << 30) : scopes.back();
            a.function = functions.empty() ? std::string() : functions.back();
            if (is_decl(kind)) a.id = n.text("id");
            a.name = n.text("name");
            if (kind == "BinaryOperator") a.name = n.text("opcode");
            if (const JValue* ref = n.find("referencedDecl")) {
                a.ref_id = ref->text("id");
                if (a.name.empty()) a.name = ref->text("name");
            }
            if (kind == "MemberExpr") a.ref_id = n.text("referencedMemberDecl");
            if (const JValue* t = n.find("type")) a.type = t->text("qualType");
            out.push_back(std::move(a));
        }
        const bool scope = is_scope(kind);
        const bool function = is_function(kind);
        if (scope) scopes.push_back(end);
        if (function) functions.push_back(n.text("name"));
        if (const JValue* inner = n.find("inner"))
            for (const JValue& c : inner->arr) node(c);
        if (function) functions.pop_back();
        if (scope) scopes.pop_back();
    }
};

}  // namespace

std::vector<AstNode> parse_ast(const std::string& source_path, std::string* error) {
    const auto slash = source_path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : source_path.substr(0, slash);
    const std::string cmd = clang_exe(true) + " -x c++ -std=c++17 -fsyntax-only -Xclang -ast-dump=json -I\"" + dir + "\" \"" + source_path + "\"";

    Scanner scanner(base_lower(source_path));
    const ProcessResult pr = run_process(cmd, 120000, false, [&](const char* p, std::size_t n) { scanner.feed(p, n); });

    std::vector<AstNode> nodes;
    if (!pr.started) {
        if (error) *error = "Clang wasn't found. Add \"C++ Clang tools for Windows\" in the Visual Studio Installer.";
        return nodes;
    }
    Walker walker{nodes};
    for (const std::string& d : scanner.decls) {
        JParser parser(d);
        walker.node(parser.parse());
    }
    if (nodes.empty() && error) *error = pr.timed_out ? "Clang took too long to read this file" : "Clang found no declarations in this file";
    else if (pr.exit_code != 0 && error) *error = "Clang reported errors in this file; what it understood is shown";

    std::unordered_map<std::string, int> by_id;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        if (!nodes[static_cast<std::size_t>(i)].id.empty()) by_id[nodes[static_cast<std::size_t>(i)].id] = i;
    for (AstNode& n : nodes) {
        if (n.ref_id.empty()) continue;
        auto it = by_id.find(n.ref_id);
        if (it != by_id.end()) n.dependencies.push_back(it->second);
    }
    return nodes;
}

}  // namespace sparring
