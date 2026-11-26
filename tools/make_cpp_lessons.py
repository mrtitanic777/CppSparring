"""Writes the C++ lesson track (lessons/cppNN_*.json) and, beside it in the scratchpad, each lesson's worked solution for checking."""
import json
import os
import sys

out_dir, check_dir = sys.argv[1], sys.argv[2]
os.makedirs(check_dir, exist_ok=True)

INC = "#include <iostream>\n"

L = []

def lesson(n, slug, course, title, instructions, task, starter, hint, expected, solution, inp="", practice=""):
    L.append(dict(n=n, slug=slug, course=course, title=title, instructions=instructions, task=task,
                  starter=starter, hint=hint, expected=expected, solution=solution, input=inp, practice=practice))

# A lesson about a kind of bug: the starter has the bug, and the fix swaps `bad` for `good`.
def bug_lesson(n, slug, title, instructions, task, starter, bad, good, hint, expected, practice):
    assert starter.count(bad) == 1
    lesson(n, slug, 13, title, instructions, task, starter, hint + "\n\n" + starter_line(starter.replace(bad, good), good),
           expected, starter.replace(bad, good), practice=practice)

def starter_line(code, part):
    return next(line for line in code.split("\n") if part in line)

lesson(1, "hello", 10, "Hello, C++",
       ["Every C++ program starts in main. The computer runs the lines inside its { } from top to bottom.",
        "std::cout << \"...\"; prints the words between the quotes.",
        "\"\\n\" means 'new line', like pressing Enter.",
        "Every line that does something ends with a semicolon ;"],
       "Make the program print exactly:  Hello, C++!",
       INC + "\nint main() {\n    // print Hello, C++! here\n\n    return 0;\n}\n",
       "Use std::cout with the words in quotes:\n\n    std::cout << \"Hello, C++!\\n\";",
       "Hello, C++!",
       INC + "\nint main() {\n    std::cout << \"Hello, C++!\\n\";\n    return 0;\n}\n")

lesson(2, "boxes", 10, "Boxes (variables)",
       ["A variable is a named box that holds a value. Its type says what can go inside.",
        "- int holds whole numbers, like 3.",
        "- double holds numbers with a decimal point, like 2.5.",
        "- std::string holds words. It needs #include <string>.",
        "You can print a box by putting its name after <<, with no quotes."],
       "Make a box called pies that holds 2.5, so the program prints:  I have 3 apples and 2.5 pies",
       INC + "\nint main() {\n    int apples = 3;\n    // make a double box called pies here\n\n    std::cout << \"I have \" << apples << \" apples and \" << pies << \" pies\\n\";\n    return 0;\n}\n",
       "A decimal number goes in a double box:\n\n    double pies = 2.5;",
       "I have 3 apples and 2.5 pies",
       INC + "\nint main() {\n    int apples = 3;\n    double pies = 2.5;\n    std::cout << \"I have \" << apples << \" apples and \" << pies << \" pies\\n\";\n    return 0;\n}\n")

lesson(3, "changing_a_box", 10, "Changing a box",
       ["The = sign is not 'equals' like in math. It means 'gets a copy of'.",
        "Read it right to left: score = score + 5; works out score + 5, then puts the answer back into score.",
        "So the box on the left changes, and the right side is where the value comes from."],
       "Add one line so score goes from 10 to 15. The program should print:  Score: 15",
       INC + "\nint main() {\n    int score = 10;\n    // add 5 to score here\n\n    std::cout << \"Score: \" << score << \"\\n\";\n    return 0;\n}\n",
       "Work it out on the right, store it on the left:\n\n    score = score + 5;",
       "Score: 15",
       INC + "\nint main() {\n    int score = 10;\n    score = score + 5;\n    std::cout << \"Score: \" << score << \"\\n\";\n    return 0;\n}\n")

lesson(4, "math", 10, "Math with whole numbers",
       ["C++ can do + (add), - (take away), * (times) and / (divide).",
        "Careful: dividing two ints gives an int. 17 / 5 is 3, not 3.4. The leftover is thrown away.",
        "% gives the leftover (the remainder): 17 % 5 is 2."],
       "Fill in the two blanks so the program prints:  17 / 5 = 3 remainder 2",
       INC + "\nint main() {\n    int a = 17;\n    int b = 5;\n    int whole = 0;   // put a / b here\n    int left = 0;    // put a % b here\n    std::cout << a << \" / \" << b << \" = \" << whole << \" remainder \" << left << \"\\n\";\n    return 0;\n}\n",
       "Use / for how many whole times, and % for the leftover:\n\n    int whole = a / b;\n    int left = a % b;",
       "17 / 5 = 3 remainder 2",
       INC + "\nint main() {\n    int a = 17;\n    int b = 5;\n    int whole = a / b;\n    int left = a % b;\n    std::cout << a << \" / \" << b << \" = \" << whole << \" remainder \" << left << \"\\n\";\n    return 0;\n}\n")

lesson(5, "if_else", 10, "Choices (if / else)",
       ["if (something is true) { ... } runs the lines inside only when the question is true.",
        "else { ... } runs when it was false.",
        "Questions use > (bigger), < (smaller), >= , <= , == (the same) and != (not the same).",
        "Remember: == asks a question, but a single = changes a box."],
       "Finish the if so the program prints:  15 is big",
       INC + "\nint main() {\n    int n = 15;\n    if (/* is n bigger than 10? */) {\n        std::cout << n << \" is big\\n\";\n    } else {\n        std::cout << n << \" is small\\n\";\n    }\n    return 0;\n}\n",
       "Bigger than is >:\n\n    if (n > 10) {",
       "15 is big",
       INC + "\nint main() {\n    int n = 15;\n    if (n > 10) {\n        std::cout << n << \" is big\\n\";\n    } else {\n        std::cout << n << \" is small\\n\";\n    }\n    return 0;\n}\n")

lesson(6, "for_loop", 10, "Loops (for)",
       ["A for loop repeats some lines. It has three parts in its ( ):",
        "- where to start: int i = 1",
        "- when to keep going: i <= 5",
        "- how to step: i = i + 1",
        "Change one part and the loop runs a different number of times."],
       "Make the loop print the numbers 1 to 5, one per line.",
       INC + "\nint main() {\n    for (int i = 1; /* keep going while... */; i = i + 1) {\n        std::cout << i << \"\\n\";\n    }\n    return 0;\n}\n",
       "Keep going while i is 5 or less:\n\n    for (int i = 1; i <= 5; i = i + 1) {",
       "1\n2\n3\n4\n5",
       INC + "\nint main() {\n    for (int i = 1; i <= 5; i = i + 1) {\n        std::cout << i << \"\\n\";\n    }\n    return 0;\n}\n")

lesson(7, "while_loop", 10, "Loops (while)",
       ["while (question) { ... } keeps repeating as long as the question is true.",
        "Something inside must change, or the question stays true forever and the loop never ends!"],
       "Add the missing line so it counts down and prints:  3 2 1 Go!  (each on its own line)",
       INC + "\nint main() {\n    int count = 3;\n    while (count > 0) {\n        std::cout << count << \"\\n\";\n        // make count one smaller here\n    }\n    std::cout << \"Go!\\n\";\n    return 0;\n}\n",
       "Take one away each time round:\n\n        count = count - 1;",
       "3\n2\n1\nGo!",
       INC + "\nint main() {\n    int count = 3;\n    while (count > 0) {\n        std::cout << count << \"\\n\";\n        count = count - 1;\n    }\n    std::cout << \"Go!\\n\";\n    return 0;\n}\n")

lesson(8, "reading_input", 10, "Reading what you type (cin)",
       ["std::cin >> n; waits for the person to type something, then puts it into the box n.",
        "Here, what the program reads comes from the answers box under the Run button. It starts with 7. Try other numbers!",
        "Notice the arrows: << sends words out to the screen, >> brings what was typed in."],
       "Read a number into n. With the answer 7, the program shows what you typed and then:  Double is 14",
       INC + "\nint main() {\n    int n = 0;\n    std::cout << \"Type a number: \";\n    // read a number into n here\n\n    std::cout << \"Double is \" << n * 2 << \"\\n\";\n    return 0;\n}\n",
       "Bring the typed number into the box:\n\n    std::cin >> n;",
       "Type a number: 7\nDouble is 14",
       INC + "\nint main() {\n    int n = 0;\n    std::cout << \"Type a number: \";\n    std::cin >> n;\n    std::cout << \"Double is \" << n * 2 << \"\\n\";\n    return 0;\n}\n",
       "7")

lesson(1, "jobs", 11, "Jobs (functions)",
       ["A function is a job with a name. You write it once and use it as many times as you like.",
        "int add(int a, int b) means: a job called add, it needs two whole numbers, and it hands back a whole number.",
        "return hands the answer back to whoever asked."],
       "Finish the add job so the program prints:  2 + 3 = 5",
       INC + "\nint add(int a, int b) {\n    // hand back a plus b\n    return 0;\n}\n\nint main() {\n    std::cout << \"2 + 3 = \" << add(2, 3) << \"\\n\";\n    return 0;\n}\n",
       "Hand back the sum:\n\n    return a + b;",
       "2 + 3 = 5",
       INC + "\nint add(int a, int b) {\n    return a + b;\n}\n\nint main() {\n    std::cout << \"2 + 3 = \" << add(2, 3) << \"\\n\";\n    return 0;\n}\n")

lesson(2, "references", 11, "The real box (&)",
       ["When you give a box to a job, the job normally gets a copy. Changing the copy doesn't change yours.",
        "Put & after the type, like int& n, and the job gets the real box instead.",
        "This is the same bug as the Piggy bank program in Practice!"],
       "This program has a bug: double_it changes a copy. Fix it so it prints:  Before: 4  then  After: 8",
       INC + "\nvoid double_it(int n) {\n    n = n * 2;\n}\n\nint main() {\n    int x = 4;\n    std::cout << \"Before: \" << x << \"\\n\";\n    double_it(x);\n    std::cout << \"After: \" << x << \"\\n\";\n    return 0;\n}\n",
       "Give the job the real box by adding &:\n\n    void double_it(int& n) {",
       "Before: 4\nAfter: 8",
       INC + "\nvoid double_it(int& n) {\n    n = n * 2;\n}\n\nint main() {\n    int x = 4;\n    std::cout << \"Before: \" << x << \"\\n\";\n    double_it(x);\n    std::cout << \"After: \" << x << \"\\n\";\n    return 0;\n}\n",
       practice="piggy_bank.cpp")

lesson(3, "const", 11, "Promises (const)",
       ["const in front of a box is a promise: this box will never change.",
        "If any line tries to change it, the computer refuses to build the program. That's a good thing: it caught a mistake!",
        "Run it first and read what the computer says."],
       "The program breaks its own promise. Delete the line that changes lives, so it builds and prints:  Lives: 3",
       INC + "\nint main() {\n    const int lives = 3;\n    lives = lives + 1;\n    std::cout << \"Lives: \" << lives << \"\\n\";\n    return 0;\n}\n",
       "A const box can't change, so remove this line:\n\n    lives = lives + 1;",
       "Lives: 3",
       INC + "\nint main() {\n    const int lives = 3;\n    std::cout << \"Lives: \" << lives << \"\\n\";\n    return 0;\n}\n")

lesson(4, "blueprints", 11, "Blueprints (struct)",
       ["A struct is a blueprint that bundles boxes together into one thing.",
        "struct Pet { std::string name; int hunger; }; says every Pet has a name and a hunger number.",
        "Reach a box inside with a dot: rex.name, rex.hunger."],
       "Make the program print:  Rex is 3 hungry",
       "#include <iostream>\n#include <string>\n\nstruct Pet {\n    std::string name;\n    int hunger;\n};\n\nint main() {\n    Pet rex{\"Rex\", 3};\n    // print the name, then \" is \", then the hunger, then \" hungry\"\n\n    return 0;\n}\n",
       "Use the dot to reach each box:\n\n    std::cout << rex.name << \" is \" << rex.hunger << \" hungry\\n\";",
       "Rex is 3 hungry",
       "#include <iostream>\n#include <string>\n\nstruct Pet {\n    std::string name;\n    int hunger;\n};\n\nint main() {\n    Pet rex{\"Rex\", 3};\n    std::cout << rex.name << \" is \" << rex.hunger << \" hungry\\n\";\n    return 0;\n}\n")

VEC = "#include <iostream>\n#include <vector>\n"
STR = "#include <iostream>\n#include <string>\n"

lesson(1, "lists", 12, "Lists (vector)",
       ["A vector is a list of boxes, all of the same type. std::vector<int> scores{3, 7, 5}; makes a list of three whole numbers.",
        "It needs #include <vector>.",
        "scores.push_back(9); adds a new box to the end of the list.",
        "scores.size() tells you how many boxes are in the list right now."],
       "Add 9 to the end of the list, so the program prints:  4 scores",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    // add 9 to the end of scores here\n\n    std::cout << scores.size() << \" scores\\n\";\n    return 0;\n}\n",
       "push_back adds to the end:\n\n    scores.push_back(9);",
       "4 scores",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    scores.push_back(9);\n    std::cout << scores.size() << \" scores\\n\";\n    return 0;\n}\n")

lesson(2, "loop_over_list", 12, "Going through a list",
       ["for (int s : scores) { ... } runs the lines once for every box in the list. Each time round, s holds the next value.",
        "This is the easiest way to look at everything in a list, one at a time.",
        "total += s; is a short way to write total = total + s;"],
       "Add up every score, so the program prints:  Total: 15",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    int total = 0;\n    for (int s : scores) {\n        // add s to total here\n    }\n    std::cout << \"Total: \" << total << \"\\n\";\n    return 0;\n}\n",
       "Add each score into the total:\n\n        total += s;",
       "Total: 15",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    int total = 0;\n    for (int s : scores) {\n        total += s;\n    }\n    std::cout << \"Total: \" << total << \"\\n\";\n    return 0;\n}\n")

lesson(3, "counting_from_zero", 12, "Counting from zero",
       ["Each box in a list has a number, called its index. The first box is number 0, not 1!",
        "scores[0] is the first box. In a list of 3, the boxes are 0, 1 and 2.",
        "So the last box is scores[scores.size() - 1]. Asking for scores[3] in a list of 3 goes past the end: a classic bug."],
       "Print the first and the last score, so the program prints:  First: 3 Last: 5  (each on its own line)",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    std::cout << \"First: \" << scores[0] << \"\\n\";\n    // print \"Last: \" and the last score here\n\n    return 0;\n}\n",
       "The last index is one less than the size:\n\n    std::cout << \"Last: \" << scores[scores.size() - 1] << \"\\n\";",
       "First: 3\nLast: 5",
       VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    std::cout << \"First: \" << scores[0] << \"\\n\";\n    std::cout << \"Last: \" << scores[scores.size() - 1] << \"\\n\";\n    return 0;\n}\n")

lesson(4, "words", 12, "Words (string)",
       ["std::string holds text, like a name. It needs #include <string>.",
        "+ glues two pieces of text together: \"Hi \" + name.",
        "name.size() tells you how many letters it has."],
       "Make the program print:  Hello, Sam! Your name has 3 letters.",
       STR + "\nint main() {\n    std::string name = \"Sam\";\n    std::string greeting = \"Hello, \" + name + \"!\";\n    // print greeting, then \" Your name has \", the size of name, and \" letters.\"\n\n    return 0;\n}\n",
       "Print the pieces one after another:\n\n    std::cout << greeting << \" Your name has \" << name.size() << \" letters.\\n\";",
       "Hello, Sam! Your name has 3 letters.",
       STR + "\nint main() {\n    std::string name = \"Sam\";\n    std::string greeting = \"Hello, \" + name + \"!\";\n    std::cout << greeting << \" Your name has \" << name.size() << \" letters.\\n\";\n    return 0;\n}\n")

lesson(5, "comparing_words", 12, "Comparing words",
       ["std::cin >> name; reads one word that the person types.",
        "== asks if two pieces of text are exactly the same. \"Max\" and \"max\" are different: capital letters count!",
        "The answers box starts with Max. Try another name too."],
       "Finish the if, so typing Max prints:  Hi Max, welcome back!",
       STR + "\nint main() {\n    std::string name;\n    std::cout << \"Name? \";\n    std::cin >> name;\n    if (/* is name Max? */) {\n        std::cout << \"Hi Max, welcome back!\\n\";\n    } else {\n        std::cout << \"Nice to meet you, \" << name << \"!\\n\";\n    }\n    return 0;\n}\n",
       "Two equals signs ask the question:\n\n    if (name == \"Max\") {",
       "Name? Max\nHi Max, welcome back!",
       STR + "\nint main() {\n    std::string name;\n    std::cout << \"Name? \";\n    std::cin >> name;\n    if (name == \"Max\") {\n        std::cout << \"Hi Max, welcome back!\\n\";\n    } else {\n        std::cout << \"Nice to meet you, \" << name << \"!\\n\";\n    }\n    return 0;\n}\n",
       "Max")

lesson(6, "rock_paper_scissors", 12, "Rock, paper, scissors",
       ["Now put it together: read a move, compare words, and choose what to print.",
        "&& means 'and': both questions must be true. (you == \"rock\" && computer == \"scissors\") is a win.",
        "The computer always picks scissors here, so you can test it. The answers box starts with rock."],
       "Finish the winning check, so typing rock prints the computer's move and then:  You win!",
       STR + "\nint main() {\n    std::string you;\n    std::string computer = \"scissors\";\n    std::cout << \"Your move? \";\n    std::cin >> you;\n    std::cout << \"Computer picks \" << computer << \"\\n\";\n    if (you == computer) {\n        std::cout << \"A tie!\\n\";\n    } else if (/* you rock AND computer scissors */) {\n        std::cout << \"You win!\\n\";\n    } else {\n        std::cout << \"Computer wins!\\n\";\n    }\n    return 0;\n}\n",
       "Both must be true, so use &&:\n\n    } else if (you == \"rock\" && computer == \"scissors\") {",
       "Your move? rock\nComputer picks scissors\nYou win!",
       STR + "\nint main() {\n    std::string you;\n    std::string computer = \"scissors\";\n    std::cout << \"Your move? \";\n    std::cin >> you;\n    std::cout << \"Computer picks \" << computer << \"\\n\";\n    if (you == computer) {\n        std::cout << \"A tie!\\n\";\n    } else if (you == \"rock\" && computer == \"scissors\") {\n        std::cout << \"You win!\\n\";\n    } else {\n        std::cout << \"Computer wins!\\n\";\n    }\n    return 0;\n}\n",
       "rock")

# ---- C++ 4: bugs to watch for (one for each kind of bug Practice and Fix it use; "practice" is a program that has it)
bug_lesson(1, "first_value", "Give every box a first value",
           ["int total; makes a box but puts nothing in it. It still holds something: junk left over in the computer's memory.",
            "Adding to junk gives more junk. Run it and look at the strange number it prints!",
            "int total = 0; makes the box and puts 0 in it straight away. Always give a box a first value."],
           "Fix the total so the program prints:  We ate 14 slices",
           VEC + "\nint main() {\n    std::vector<int> slices{3, 5, 2, 4};\n    int total;\n    for (int s : slices) {\n        total += s;\n    }\n    std::cout << \"We ate \" << total << \" slices\\n\";\n    return 0;\n}\n",
           "int total;", "int total = 0;", "Start the box at 0:", "We ate 14 slices", "pizza_party.cpp")

bug_lesson(2, "whole_numbers", "Why 7 / 2 is 3",
           ["When you divide a whole number by a whole number, C++ gives back a whole number. It throws away the part after the dot, so 7 / 2 is 3.",
            "If one of them is a decimal, like 2.0, the answer is a decimal too: 7 / 2.0 is 3.5.",
            "Putting the answer in a double box is not enough. The dividing already happened with whole numbers!"],
           "14 slices shared by 4 people should be 3.5 each. Fix it so the program prints:  That is 3.5 slices each",
           INC + "\nint main() {\n    int total = 14;\n    double each = total / 4;\n    std::cout << \"That is \" << each << \" slices each\\n\";\n    return 0;\n}\n",
           "total / 4;", "total / 4.0;", "Make the 4 a decimal:", "That is 3.5 slices each", "pizza_party.cpp")

bug_lesson(3, "end_of_list", "Stop at the end of the list",
           ["A list of 3 has boxes 0, 1 and 2. There is no box 3!",
            "i <= scores.size() lets i reach 3, so the loop asks for a box that isn't there. The program crashes.",
            "i < scores.size() stops at 2, the last real box."],
           "This program crashes. Fix the loop so it prints:  Total: 15",
           VEC + "\nint main() {\n    std::vector<int> scores{3, 7, 5};\n    int total = 0;\n    for (std::size_t i = 0; i <= scores.size(); i = i + 1) {\n        total += scores[i];\n    }\n    std::cout << \"Total: \" << total << \"\\n\";\n    return 0;\n}\n",
           "i <= scores", "i < scores", "Stop before the size, not at it:", "Total: 15", "pizza_party.cpp")

bug_lesson(4, "less_or_equal", "< or <=",
           ["i < 5 means 'smaller than 5', so the loop stops before 5.",
            "i <= 5 means 'smaller than or the same as 5', so 5 gets a turn too.",
            "Mixing these up makes a loop run one time too few or one time too many. It's called an off-by-one bug, and everyone makes it."],
           "The loop should count all the way to 5. Fix it so the program prints 1 2 3 4 5, each on its own line.",
           INC + "\nint main() {\n    for (int i = 1; i < 5; i = i + 1) {\n        std::cout << i << \"\\n\";\n    }\n    return 0;\n}\n",
           "i < 5", "i <= 5", "Let 5 have a turn:", "1\n2\n3\n4\n5", "sum_to_ten.cpp")

bug_lesson(5, "equals", "= puts, == asks",
           ["One = puts a value in a box: guess = 7 makes guess hold 7.",
            "Two == asks a question: guess == 7 checks if guess holds 7, and changes nothing.",
            "if (guess = secret) doesn't ask anything. It copies the secret into guess, so the answer always looks right!"],
           "The guess is 4 and the secret is 7, but it says you got it. Fix it so the program prints:  Not this time!",
           INC + "\nint main() {\n    int secret = 7;\n    int guess = 4;\n    if (guess = secret) {\n        std::cout << \"You got it!\\n\";\n    } else {\n        std::cout << \"Not this time!\\n\";\n    }\n    return 0;\n}\n",
           "guess = secret", "guess == secret", "Ask with two equals signs:", "Not this time!", "secret_number.cpp")

bug_lesson(6, "catch_the_answer", "Catch the answer",
           ["return hands an answer back. But someone has to catch it!",
            "double_it(score); works out the answer and then drops it. score stays the same.",
            "score = double_it(score); catches the answer and puts it in the box."],
           "The score should go from 4 to 8. Fix it so the program prints:  Score: 8",
           INC + "\nint double_it(int n) {\n    return n * 2;\n}\n\nint main() {\n    int score = 4;\n    double_it(score);\n    std::cout << \"Score: \" << score << \"\\n\";\n    return 0;\n}\n",
           "    double_it(score);", "    score = double_it(score);", "Put the answer back in the box:", "Score: 8", "double_it.cpp")

track_number = {10: 0, 11: 8, 12: 12, 13: 18}
for x in L:
    number = x["n"] + track_number[x["course"]]
    name = "cpp%02d_%s" % (number, x["slug"])
    data = {"title": x["title"], "language": "cpp", "course": x["course"], "number": number,
            "instructions": x["instructions"], "task": x["task"], "starter_code": x["starter"], "hint": x["hint"],
            "expected_output": x["expected"]}
    if x["input"]:
        data["input"] = x["input"]
    if x["practice"]:
        data["practice"] = x["practice"]
    with open(os.path.join(out_dir, name + ".json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    with open(os.path.join(check_dir, name + ".cpp"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(x["solution"])
    with open(os.path.join(check_dir, name + ".expected"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(x["expected"])
    with open(os.path.join(check_dir, name + ".input"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(x["input"])
print(len(L), "lessons written")
