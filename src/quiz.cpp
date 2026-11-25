#include "quiz.hpp"

namespace sparring {

const std::vector<QuizCard>& quiz_cards() {
    static const std::vector<QuizCard> all = {
        // ---- from CodingTeacher's NOTES.md "Test Yourself"
        {"types", "Which type holds a whole number, like 33?", nullptr, {"int", "double", "char", "float"},
         "int is for whole numbers. double and float hold numbers with a decimal point, and char holds one letter."},
        {"ascii", "What number does 'A' really store?", nullptr, {"65", "1", "97", "0"},
         "A char is secretly a number, its ASCII code. 'A' is 65 and 'a' is 97."},
        {"quotes", "What's the difference between 'A' and \"A\"?", nullptr,
         {"'A' is one letter; \"A\" is text (a string)", "There is no difference", "'A' is a bigger letter", "\"A\" is a number"},
         "Single quotes hold exactly one letter. Double quotes hold a string, which is a row of letters."},
        {"copy", "Afterwards, what are x and y?", "int x = 5;\nint y = 9;\nx = y;", {"x is 9 and y is 9", "x is 5 and y is 5", "x is 9 and y is 5", "x is 5 and y is 9"},
         "= copies the right side into the left box. Read it right to left: x gets a copy of y. y doesn't change."},
        {"chop", "What is n?", "double d = 3.99;\nint n = (int)d;", {"3", "4", "3.99", "0"},
         "Turning a decimal into an int chops the decimal off. It does not round, so 3.99 becomes 3."},
        {"names", "Can a box be called 2score?", nullptr,
         {"No, a name can't start with a digit", "Yes, any name works", "Only in C++", "Only if it holds a number"},
         "Names use letters, digits and _, but they can't start with a digit. score2 is fine; 2score is not."},
        {"newline", "What does \\n do inside printf?", nullptr, {"Starts a new line", "Prints the letter n", "Ends the program", "Adds a space"},
         "\\n is an instruction, not text: it moves the printing down to the next line."},
        {"h", "Which letter prints?", "printf(\"%c\\n\", 72);", {"H", "72", "c", "A"},
         "72 is the ASCII code for H, and %c shows a number as its letter."},
        {"cast", "Which line is an explicit cast (you asked for it on purpose)?", nullptr, {"a = (int)b;", "a = b;", "int a;", "b = 3.7;"},
         "Writing the type in brackets, (int)b, says: I meant to convert this. a = b; converts too, but silently."},
        {"source", "After this, what is b?", "double b = 3.7;\nint a = (int)b;", {"3.7", "3", "4", "0"},
         "Casting makes a copy. The copy that landed in a got chopped to 3, but b is still 3.7."},
        // ---- the ideas in the break-it programs
        {"ref", "What does the & mean here?", "void add_coins(int& bank)", {"The job gets the real box, not a copy", "Add two numbers together", "bank is some text", "Nothing at all"},
         "With &, the job works on the real box. Without it, the job only gets a copy."},
        {"const", "What does const in front of a box mean?", "const int lives = 3;", {"The box can never change", "The box always holds 0", "The box gets copied", "The box is hidden"},
         "const is a promise: this box will never change. The computer stops anyone who tries."},
        {"eq", "What's the difference between = and ==?", nullptr,
         {"= puts a value in; == asks if two are the same", "There is no difference", "== puts a value in; = asks", "= adds two numbers"},
         "One = copies the right side into the left box. Two == ask a yes-or-no question: are these the same?"},
        {"loop3", "How many times does this loop run?", "for (int i = 1; i <= 3; i = i + 1)", {"3 times", "2 times", "4 times", "Forever"},
         "i is 1, then 2, then 3. When i becomes 4, i <= 3 is no longer true, so the loop stops."},
        {"forever", "What happens if you delete the line count = count - 1;?", "int count = 3;\nwhile (count > 0) {\n    count = count - 1;\n}",
         {"The loop never stops", "It runs 3 times", "It won't build", "count becomes 0"},
         "count would stay 3 forever, so count > 0 is always true and the loop goes round and round."},
        {"return", "What does return do in a job (function)?", nullptr, {"Hands the answer back", "Starts the job again", "Prints the answer", "Deletes the box"},
         "return hands the job's answer back to whoever asked for it."},
        {"struct", "What is a struct (a blueprint)?", nullptr,
         {"A way to bundle boxes into one thing", "A kind of loop", "A type of number", "A way to print"},
         "A struct bundles boxes together. A Pet can hold a name and a hunger number at the same time."},
        {"main", "Where does every C and C++ program start running?", nullptr, {"In main", "On the first line of the file", "In the last job", "At #include"},
         "The computer always starts at main, no matter where it sits in the file."},
    };
    return all;
}

std::array<int, 4> answer_order(std::size_t index) {
    // a different place for the right answer on each card, the same every time
    static const std::array<std::array<int, 4>, 4> orders = {{{1, 0, 3, 2}, {2, 3, 0, 1}, {0, 2, 1, 3}, {3, 1, 2, 0}}};
    return orders[(index * 7 + 1) % 4];
}

}  // namespace sparring
