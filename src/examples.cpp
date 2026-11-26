#include "examples.hpp"

#include <filesystem>
#include <fstream>

namespace sparring {

const std::vector<Level>& levels() {
    static const std::vector<Level> all = {
        {1, "Boxes", "Boxes (variables) hold numbers. You can change what's inside."},
        {2, "Jobs", "Jobs (functions) do one thing when you ask them to."},
        {3, "Loops", "Loops do the same thing again and again."},
        {4, "Blueprints", "Blueprints (structs) bundle boxes together into one thing."},
        {5, "Lists & games", "Lists (vectors) hold many values; strings hold words."},
    };
    return all;
}

const std::vector<Example>& examples() {
    static const std::vector<Example> all = {
        // ---- level 1: boxes
        {1, "robot_age.cpp", "Robot birthday", "A robot turns one year older.", L'\xE99A',
         R"(#include <iostream>

int main() {
    int age = 9;
    age = age + 1;
    std::cout << "Next year the robot is " << age << "\n";
    return 0;
}
)"},
        {1, "snack_count.cpp", "Snack count", "Works out how many snacks a party needs.", L'\xE719',
         R"(#include <iostream>

int main() {
    const int snacks_each = 3;
    int kids = 4;
    int total = snacks_each * kids;
    std::cout << "We need " << total << " snacks\n";
    return 0;
}
)"},
        // ---- level 2: jobs
        {2, "piggy_bank.cpp", "Piggy bank", "Adds coins to a bank and counts them.", L'\xE825',
         R"(#include <iostream>

void add_coins(int& bank, int coins) {
    bank = bank + coins;
}

int main() {
    int my_bank = 10;
    add_coins(my_bank, 5);
    add_coins(my_bank, 3);
    std::cout << "Coins in my bank: " << my_bank << "\n";
    return 0;
}
)"},
        {2, "secret_number.cpp", "Secret number", "Checks if a guess matches the secret number.", L'\xE72E',
         R"(#include <iostream>

bool is_secret(int guess) {
    const int secret = 7;
    return guess == secret;
}

int main() {
    std::cout << "Is 3 the secret? " << (is_secret(3) ? "yes" : "no") << "\n";
    std::cout << "Is 7 the secret? " << (is_secret(7) ? "yes" : "no") << "\n";
    return 0;
}
)"},
        {2, "double_it.cpp", "Double it", "A job that doubles any number you give it.", L'\xE8EF',
         R"(#include <iostream>

int double_it(int n) {
    return n * 2;
}

int main() {
    int score = double_it(4);
    std::cout << "Double 4 is " << score << "\n";
    std::cout << "Double 10 is " << double_it(10) << "\n";
    return 0;
}
)"},
        // ---- level 3: loops
        {3, "times_table.cpp", "Times table", "Prints the 3 times table from 1 to 5.", L'\xE8EF',
         R"(#include <iostream>

int times(int a, int b) {
    return a * b;
}

int main() {
    for (int i = 1; i <= 5; i = i + 1) {
        std::cout << "3 x " << i << " = " << times(3, i) << "\n";
    }
    return 0;
}
)"},
        {3, "countdown.cpp", "Rocket countdown", "Counts down from 5, then blasts off.", L'\xE7C1',
         R"(#include <iostream>

int main() {
    int count = 5;
    while (count > 0) {
        std::cout << count << "...\n";
        count = count - 1;
    }
    std::cout << "Blast off!\n";
    return 0;
}
)"},
        {3, "sum_to_ten.cpp", "Add them up", "Adds every number from 1 to 10.", L'\xE8EF',
         R"(#include <iostream>

int main() {
    int total = 0;
    for (int i = 1; i <= 10; i = i + 1) {
        total += i;
    }
    std::cout << "1 + 2 + ... + 10 = " << total << "\n";
    return 0;
}
)"},
        {3, "guess_number.cpp", "Guess my number", "Keeps asking until you guess the secret number.", L'\xE897',
         R"(#include <iostream>

int main() {
    const int secret = 5;
    int guess = 0;
    int tries = 0;
    while (guess != secret) {
        std::cout << "Your guess: ";
        std::cin >> guess;
        tries = tries + 1;
        if (guess < secret) {
            std::cout << "Too low!\n";
        }
        if (guess > secret) {
            std::cout << "Too high!\n";
        }
    }
    std::cout << "You got it in " << tries << " tries!\n";
    return 0;
}
)",
         "3\n8\n5\n"},
        // ---- level 4: blueprints
        {4, "pet.cpp", "Feed the pet", "A pet gets less hungry when you feed it.", L'\xEB51',
         R"(#include <iostream>
#include <string>

struct Pet {
    std::string name;
    int hunger;
};

void feed(Pet& pet) {
    pet.hunger = pet.hunger - 2;
}

int main() {
    Pet cat{"Whiskers", 5};
    feed(cat);
    std::cout << cat.name << " hunger: " << cat.hunger << "\n";
    return 0;
}
)"},
        {4, "bank_account.cpp", "Bank account", "Puts money in, takes some out, and shows what's left.", L'\xE825',
         R"(#include <iostream>
#include <string>

struct Account {
    std::string owner;
    int money;
};

void put_in(Account& acct, int amount) {
    acct.money = acct.money + amount;
}

void take_out(Account& acct, int amount) {
    acct.money = acct.money - amount;
}

void show(const Account& acct) {
    std::cout << acct.owner << " has " << acct.money << " coins\n";
}

int main() {
    Account mine{"Sam", 20};
    put_in(mine, 15);
    take_out(mine, 5);
    show(mine);
    return 0;
}
)"},
        {4, "game_over.cpp", "Game over", "A player loses a life each time they get hit.", L'\xE7FC',
         R"(#include <iostream>
#include <string>

struct Player {
    std::string name;
    int lives;
};

void get_hit(Player& p) {
    p.lives = p.lives - 1;
}

int main() {
    Player hero{"Max", 3};
    for (int round = 1; round <= 3; round = round + 1) {
        get_hit(hero);
        std::cout << "Round " << round << ": " << hero.lives << " lives left\n";
    }
    if (hero.lives == 0) {
        std::cout << "Game over!\n";
    }
    return 0;
}
)"},
        // ---- level 5: lists and games
        {5, "high_scores.cpp", "High scores", "Finds the best score in a list.", L'\xE735',
         R"(#include <iostream>
#include <vector>

int main() {
    std::vector<int> scores{12, 30, 25};
    int best = 0;
    for (int s : scores) {
        if (s > best) {
            best = s;
        }
    }
    std::cout << scores.size() << " scores, the best is " << best << "\n";
    return 0;
}
)"},
        {5, "word_shout.cpp", "Word shout", "Makes a word louder with exclamation marks.", L'\xE767',
         R"(#include <iostream>
#include <string>

int main() {
    std::string word = "hello";
    std::string shout = word + "!!!";
    std::cout << shout << "\n";
    std::cout << "That is " << shout.size() << " letters long\n";
    return 0;
}
)"},
        {5, "rock_paper_scissors.cpp", "Rock paper scissors", "You type a move. The computer always picks scissors.", L'\xE7FC',
         R"(#include <iostream>
#include <string>

int main() {
    std::string you;
    std::string computer = "scissors";
    std::cout << "Your move? ";
    std::cin >> you;
    std::cout << "Computer picks " << computer << "\n";
    if (you == computer) {
        std::cout << "A tie!\n";
    } else if (you == "rock" && computer == "scissors") {
        std::cout << "You win!\n";
    } else {
        std::cout << "Computer wins!\n";
    }
    return 0;
}
)",
         "rock\n"},
    };
    return all;
}

std::vector<std::size_t> examples_in_level(int level) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < examples().size(); ++i)
        if (examples()[i].level == level) out.push_back(i);
    return out;
}

std::string write_example(const Example& e) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "CppSparring" / "examples";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path p = dir / e.file;
    std::ofstream(p, std::ios::binary) << e.source;
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace sparring
