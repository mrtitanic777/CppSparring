// countdown.cpp - counts down from three
#include <iostream>

int main() {
    for (int i = 3; i > 0; --i) {
        std::cout << i << "...\n";
    }
    std::cout << "Go!\n";
    return 0;
}
