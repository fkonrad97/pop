#include <iostream>
#include <vector>
#include <string>
#include "utils.h"

int main() {
    std::cout << "Hello world" << std::endl;

    // C++20 feature: range-based for with initializer
    for (std::string s : {"one", "two", "three"}) {
        std::cout << "Item: " << s << '\n';
    }

    // C++20 feature: using auto in structured bindings
    std::vector<std::pair<int, std::string>> data = {
        {1, "alpha"},
        {2, "beta"},
        {3, "gamma"}
    };

    for (auto [id, name] : data) {
        std::cout << id << " -> " << name << '\n';
    }

    int x = 5, y = 3;
    std::cout << "x + y = " << utils::add(x, y) << '\n';
    std::cout << "x * y = " << utils::multiply(x, y) << '\n';

    return 0;
}