#include "fix.hpp"

#include <algorithm>
#include <numeric>

#include "compile.hpp"
#include "examples.hpp"
#include "explain.hpp"
#include "lessons.hpp"
#include "mutate.hpp"

namespace sparring {

namespace {

// What was done to the program, said so the learner knows what to put back.
std::string clue(const MutationToggle& t) {
    switch (t.rule) {
    case Rule::SwapOp: return "Someone turned a " + t.op_from + " into a " + t.op_to + ".";
    case Rule::EqToAssign: return "Someone turned an == into a single =.";
    case Rule::RemoveAssign: return "Someone deleted the line that changes " + t.name + ".";
    case Rule::RemoveRef: return "Someone took the & away from " + t.name + ".";
    case Rule::RemoveConst: return "Someone took const away from " + t.name + ".";
    case Rule::RemoveReturn: return "Someone deleted the return in " + t.name + ".";
    case Rule::RemoveIgnore: return "Someone deleted cin.ignore().";
    }
    return {};
}

std::string title_of(const std::string& name) {
    for (const Example& e : examples())
        if (name == e.file) return e.title;
    return name;
}

}  // namespace

std::string fix_key(const std::string& file, const MutationToggle& t) { return "fix:" + file + "|" + t.label + "@" + std::to_string(t.line); }

FixPuzzle make_fix(const std::string& path, const std::string& name, const std::string& source, const std::vector<MutationToggle>& toggles,
                   const std::string& input, const std::set<std::string>& fixed, unsigned seed) {
    FixPuzzle out;
    const CompileResult original = compile_and_run(path, input);
    if (!original.compiled || original.crashed || original.timed_out) {
        out.error = "This program has to build and run before it can be broken and fixed.";
        return out;
    }
    const std::string expected = normalize_output(original.output);
    if (expected.empty()) {
        out.error = "This program doesn't print anything, so there's nothing to check a fix against.";
        return out;
    }

    // a shuffled order, bugs not fixed before first
    std::vector<int> order(toggles.size());
    std::iota(order.begin(), order.end(), 0);
    std::uint32_t s = seed * 2654435761u + 1u;
    for (std::size_t i = order.size(); i > 1; --i) {
        s = s * 1664525u + 1013904223u;
        std::swap(order[i - 1], order[(s >> 8) % i]);
    }
    std::stable_partition(order.begin(), order.end(), [&](int i) { return !fixed.count(fix_key(name, toggles[static_cast<std::size_t>(i)])); });

    // a bug that changes what it prints (or crashes it) is the best puzzle; one that stops it building will do
    int pick = -1, fallback = -1, tries = 0;
    Outcome how = Outcome::Different;
    for (int i : order) {
        if (tries >= 8 && fallback >= 0) break;   // keep the wait short
        const MutationToggle& t = toggles[static_cast<std::size_t>(i)];
        const std::string broken = apply_mutation(source, t);
        if (broken == source) continue;
        ++tries;
        const Outcome o = classify(original, compile_and_run_string(broken, path, input));
        if (o == Outcome::Different || o == Outcome::Crashes) {
            pick = i;
            how = o;
            break;
        }
        if (o == Outcome::WontBuild && fallback < 0) fallback = i;
    }
    if (pick < 0 && fallback >= 0) {
        pick = fallback;
        how = Outcome::WontBuild;
    }
    if (pick < 0) {
        out.error = "Every change to this program still prints the same thing, so there's no bug to fix here.";
        return out;
    }

    const MutationToggle& t = toggles[static_cast<std::size_t>(pick)];
    Lesson& l = out.lesson;
    l.id = fix_key(name, t);
    l.cpp = !(name.size() >= 2 && name.compare(name.size() - 2, 2, ".c") == 0);
    l.title = title_of(name) + " is broken!";
    const char* what = how == Outcome::WontBuild ? "Now the computer can't even build it."
                       : how == Outcome::Crashes  ? "Now it crashes or gets stuck when it runs."
                                                  : "It still builds, but now it prints the wrong thing.";
    l.instructions = {std::string("Someone changed one small thing in this program. ") + what,
                      "- Read the code and look for the part that seems wrong.", "- Change it back, then press Run.",
                      "- It's fixed when it prints exactly what IT SHOULD PRINT shows."};
    l.task = "Find the bug and fix it, so the program prints what it should again.";
    l.starter = apply_mutation(source, t);
    l.expected = expected;
    l.input = input;
    const std::string changed = line_after(source, t);
    l.hint = "Look at line " + std::to_string(t.line) + ".\n\n";
    if (!changed.empty()) l.hint += "    " + changed + "\n\n";
    l.hint += clue(t) + " Put it back.";
    l.path = path;
    out.ok = true;
    return out;
}

}  // namespace sparring
