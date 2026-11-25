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
