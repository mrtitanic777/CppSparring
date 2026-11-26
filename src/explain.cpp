#include "explain.hpp"

#include <set>

namespace sparring {

namespace {

void erase_all(std::string& s, const std::string& what) {
    for (std::size_t p = s.find(what); p != std::string::npos; p = s.find(what, p)) s.erase(p, what.size());
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// "const std::string &" -> "some text"
std::string friendly_type(std::string t) {
    for (const char* junk : {"const ", "std::", "struct ", "class ", "&"}) erase_all(t, junk);
    t = trim(t);
    if (t.rfind("vector<", 0) == 0 && t.back() == '>') {
        const std::string inner = friendly_type(t.substr(7, t.size() - 8));
        return "a list, where each box holds " + inner;
    }
    if (t == "int" || t == "long" || t == "short" || t == "unsigned int" || t == "size_t") return "a whole number";
    if (t == "double" || t == "float") return "a decimal number";
    if (t == "bool") return "a yes-or-no answer";
    if (t == "string") return "some text";
    if (t == "char") return "one letter";
    if (t == "void") return "nothing";
    return "a " + t;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += i + 1 == parts.size() ? " and " : ", ";
        out += parts[i];
    }
    return out;
}

bool is_function(const std::string& k) { return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl"; }

std::string times(int n) {
    if (n == 0) return "never used";
    if (n == 1) return "used once";
    if (n == 2) return "used twice";
    return "used " + std::to_string(n) + " times";
}

}  // namespace

std::vector<LookItem> describe(const std::vector<AstNode>& nodes) {
    std::vector<int> uses(nodes.size(), 0);
    for (const AstNode& n : nodes)
        for (int d : n.dependencies)
            if (d >= 0 && d < static_cast<int>(uses.size())) ++uses[static_cast<std::size_t>(d)];

    std::vector<LookItem> items;
    std::set<std::size_t> inside;   // variables already told about inside a job
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const AstNode& n = nodes[i];
        if (is_function(n.kind)) {
            std::vector<std::string> params;
            std::size_t j = i + 1;
            for (; j < nodes.size() && nodes[j].kind == "ParmVarDecl"; ++j) {
                const AstNode& p = nodes[j];
                const bool ref = p.type.find('&') != std::string::npos;
                const bool ro = p.type.find("const") != std::string::npos;
                params.push_back(p.name + (ref ? (ro ? " (the real one, just to read)" : " (the real one)") : " (a copy)"));
            }
            std::vector<std::string> locals;
            for (std::size_t k = j; k < nodes.size() && !is_function(nodes[k].kind) && nodes[k].kind != "CXXRecordDecl"; ++k)
                if (nodes[k].kind == "VarDecl" && !nodes[k].name.empty()) {
                    locals.push_back(nodes[k].name);
                    inside.insert(k);
                }

            LookItem it;
            it.name = n.name;
            it.line = n.line;
            const std::string ret = n.type.substr(0, n.type.find(" ("));
            if (n.name == "main") {
                it.kind = LookKind::Start;
                it.sentence = "The starting point. The computer begins running the program here.";
                it.detail = "Line " + std::to_string(n.line);
            } else {
                it.kind = LookKind::Job;
                it.sentence = params.empty() ? "A job that needs nothing to get started." : "A job. It needs " + join(params) + ".";
                if (friendly_type(ret) != "nothing") it.sentence += " It hands back " + friendly_type(ret) + ".";
                it.detail = "Line " + std::to_string(n.line) + "  ·  " + times(uses[i]);
            }
            if (!locals.empty()) it.sentence += std::string(locals.size() == 1 ? " Inside it is a box called " : " Inside it are boxes called ") + join(locals) + ".";
            items.push_back(std::move(it));
        } else if (n.kind == "CXXRecordDecl" && !n.name.empty()) {
            std::vector<std::string> fields;
            for (std::size_t j = i + 1; j < nodes.size() && nodes[j].kind != "CXXRecordDecl" && nodes[j].kind != "FunctionDecl"; ++j)
                if (nodes[j].kind == "FieldDecl") fields.push_back(nodes[j].name);
            LookItem it;
            it.kind = LookKind::Blueprint;
            it.name = n.name;
            it.line = n.line;
            it.sentence = "A blueprint for making things." + (fields.empty() ? std::string() : " Each " + n.name + " holds " + join(fields) + ".");
            it.detail = "Line " + std::to_string(n.line);
            items.push_back(std::move(it));
        } else if (n.kind == "VarDecl" && !n.name.empty() && !inside.count(i)) {
            LookItem it;
            it.kind = LookKind::Box;
            it.name = n.name;
            it.line = n.line;
            it.sentence = "A box that holds " + friendly_type(n.type) + ". Every part of the program can use it.";
            it.detail = "Line " + std::to_string(n.line) + "  ·  " + times(uses[i]);
            items.push_back(std::move(it));
        }
    }
    return items;
}

Outcome classify(const CompileResult& original, const CompileResult& changed) {
    if (!changed.compiled) return Outcome::WontBuild;
    if (changed.crashed || changed.timed_out) return Outcome::Crashes;
    if (changed.output != original.output || changed.exit_code != original.exit_code) return Outcome::Different;
    return Outcome::Same;
}

const char* outcome_title(Outcome o) {
    switch (o) {
    case Outcome::WontBuild: return "It won't build";
    case Outcome::Crashes: return "It crashes";
    case Outcome::Different: return "It prints something else";
    case Outcome::Same: return "Nothing changes";
    }
    return "";
}

const char* outcome_sub(Outcome o) {
    switch (o) {
    case Outcome::WontBuild: return "The computer can't read it";
    case Outcome::Crashes: return "It stops or gets stuck";
    case Outcome::Different: return "The answer changes";
    case Outcome::Same: return "Same answer as before";
    }
    return "";
}

std::string real_life(const MutationToggle& t) {
    switch (t.rule) {
    case Rule::RemoveRef: return "This is how a game's health bar can stay full: the damage hit a copy of the player, not the real one.";
    case Rule::RemoveConst: return "const is how a game makes sure nothing can secretly change how many lives you start with.";
    case Rule::RemoveReturn: return "An app that forgets to return its answer can show a random score or a silly price.";
    case Rule::RemoveIgnore: return "This is why a quiz app sometimes skips a question right after you type: the leftover Enter answered it.";
    case Rule::EqToAssign: return "A password check with = instead of == would let anyone in, whatever they typed.";
    case Rule::SwapOp:
        if (t.op_from[0] == '<' || t.op_from[0] == '>')
            return "This \"off by one\" mistake is why a level can say 11 coins when there are only 10. Real programmers make it all the time.";
        return "One wrong sign in a shop's math, and you get charged money instead of getting it back.";
    case Rule::RemoveAssign: return "If a game forgets the line that saves your new score, you're stuck with your old one.";
    case Rule::StartAt: return "Off by one again: start counting at the wrong number and a game skips player 1, or deals one card too many.";
    case Rule::MakeInt: return "Money apps keep the part after the dot for a reason: chop it off and a $3.99 snack costs $3.";
    case Rule::RemoveInit: return "Junk values are why a brand-new game save can show a score of -1431655766. Always give a box a first value.";
    case Rule::WholeDivide: return "Whole-number division is why sharing 7 cookies between 2 friends in code gives 3 each, and one cookie just vanishes.";
    case Rule::PastEnd: return "Reading past the end of a list is one of the most common crashes in real games and apps. Some even let hackers in.";
    }
    return "";
}

std::string plain_error(const std::string& out) {
    std::size_t p = out.find("error: ");
    std::string line;
    if (p != std::string::npos) {
        p += 7;
        line = out.substr(p, out.find('\n', p) - p);
    } else {
        line = out.substr(0, out.find('\n'));
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    // the most common complaints, said the way a beginner would say them
    auto quoted = [&](std::size_t from) {
        const std::size_t a = line.find('\'', from);
        const std::size_t b = a == std::string::npos ? a : line.find('\'', a + 1);
        return b == std::string::npos ? std::string() : line.substr(a + 1, b - a - 1);
    };
    const struct {
        const char* says;
        const char* means;
    } plain[] = {
        {"invalid operands to binary expression", "That sign can't be used with these kinds of values."},
        {"contextually convertible to 'bool'", "That isn't a yes-or-no question, so the if can't use it."},
        {"no viable conversion", "Those two kinds of values don't fit together."},
        {"expected ';'", "A semicolon ; is missing."},
        {"no matching function for call", "That job doesn't take what was given to it."},
        {"non-void function does not return a value", "This job has no return, so it never hands back an answer."},
    };
    for (const auto& p : plain)
        if (line.find(p.says) != std::string::npos) return p.means;
    if (line.find("use of undeclared identifier") != std::string::npos) return "There's no box or job called " + quoted(0) + ".";
    if (line.find("const-qualified type") != std::string::npos) return quoted(0) + " is const, so it can't be changed.";
    if (line.size() > 160) line = line.substr(0, 157) + "...";
    return line;
}

std::string explain_result(const MutationToggle& t, Outcome o, const CompileResult& changed) {
    switch (o) {
    case Outcome::WontBuild:
        return "The computer couldn't understand the changed code, so it refused to build it. It said: \"" + plain_error(changed.compile_errors) + "\"";
    case Outcome::Crashes:
        if (changed.timed_out && (t.rule == Rule::SwapOp || t.rule == Rule::RemoveAssign))
            return "The loop never reached its end, so it went round and round forever. We stopped it after 5 seconds.";
        if (changed.timed_out) return "The program got stuck and never finished, so we stopped it after 5 seconds.";
        if (t.rule == Rule::PastEnd)
            return "The loop tried to read a box past the end of the list. That box doesn't exist, so the program was stopped before it could do any harm.";
        if (t.rule == Rule::RemoveReturn)
            return "The job reached its end with no answer to hand back. The computer didn't know what to do, so the program crashed.";
        return "The program crashed. It stopped suddenly because something went badly wrong inside it.";
    case Outcome::Different:
        switch (t.rule) {
        case Rule::RemoveRef:
            return t.name + " became a copy. The job changed the copy, then the copy was thrown away, so the real one never changed.";
        case Rule::RemoveReturn: return "The job never handed back its answer, so the program used a leftover junk value instead.";
        case Rule::EqToAssign: return "A single = doesn't compare anything. It copied one value over the other, so the answer came out different.";
        case Rule::RemoveConst: return "Without const, something was allowed to change " + t.name + ", and what the program printed changed too.";
        case Rule::RemoveIgnore: return "The leftover Enter key got read as the next answer, so the program went a different way.";
        case Rule::SwapOp:
            if (t.op_from[0] == '<' || t.op_from[0] == '>')
                return "The check came out differently, so the loop or the choice ran a different number of times. That's why the answer changed.";
            return "Using " + t.op_to + " instead of " + t.op_from + " made the math come out different, so the answer changed.";
        case Rule::RemoveAssign: return t.name + " kept its old value, because the line that changed it is gone. So the answer changed.";
        case Rule::StartAt: return "The loop started at " + t.op_to + " instead of " + t.op_from + ", so it ran a different number of times and the answer changed.";
        case Rule::MakeInt: return t.name + " can only hold whole numbers now, so the part after the dot was chopped off.";
        case Rule::RemoveInit:
            return t.name + " never got a first value, so it started out holding junk (-1431655766 here), and the answer came out wrong.";
        case Rule::WholeDivide:
            return "Both sides of / were whole numbers, so the computer did whole-number division and threw the leftover away.";
        case Rule::PastEnd: return "The loop read one box past the end of the list and used whatever it found there.";
        }
        break;
    case Outcome::Same:
        switch (t.rule) {
        case Rule::RemoveRef:
            return "The job only looked at " + t.name + " and never changed it, so a copy works just as well. (Copying takes a little extra time, though.)";
        case Rule::RemoveConst:
            return "Nothing tried to change " + t.name + ", so the program works the same. const is a safety promise: it stops mistakes before they happen.";
        case Rule::RemoveReturn:
            if (t.name == "main") return "It printed exactly the same thing. main is special: if it has no return, the computer acts as if it said return 0.";
            return "It printed exactly the same thing. Nothing used the answer that " + t.name + " hands back.";
        case Rule::SwapOp:
            return "Here " + t.op_to + " happened to give the same answer as " + t.op_from + ". In another program it could matter a lot.";
        case Rule::RemoveAssign: return "Nothing used the new value of " + t.name + " afterwards, so deleting that line didn't change what was printed.";
        case Rule::RemoveInit: return t.name + " got a new value before anything read it, so its first value didn't matter here.";
        case Rule::MakeInt: return "The numbers in " + t.name + " happened to be whole anyway, so nothing was chopped off.";
        case Rule::WholeDivide: return "The numbers divided evenly, so there was no leftover to throw away.";
        default: return "It printed exactly the same thing. This change didn't matter in this program.";
        }
    }
    return "";
}

}  // namespace sparring
