#include <iostream>
#include <string>
#include <vector>

int main() {
	std::vector<std::string> strings;
	while (true) {
		std::string& input = strings.emplace_back();
		std::cout << "Enter a string: ";
		std::getline(std::cin, input);
		std::cout << "The string content is present at address: " << static_cast<const void*>(input.c_str()) << std::endl;
		std::cout << "The string content is: " << input.c_str() << std::endl;
	}
	return 0;
}
