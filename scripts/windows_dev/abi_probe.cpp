#include <iostream>
#include <string>

int main() {
    const std::string message = "Frame ABI probe";
    std::cout << message << '\n';
    return message.size() == 15 ? 0 : 1;
}
