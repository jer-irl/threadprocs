#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

int main() {
	std::vector<std::unique_ptr<std::string>> strings;
	while (true) {
		// unique_ptr to avoid small string optimization changing the string address when the vector reallocs
		auto input = std::make_unique<std::string>();
		std::cout << "Enter a string: ";
		if (!std::getline(std::cin, *input)) {
			break;
		}
		std::cout << "The string content is present at address: " << static_cast<const void*>(input->c_str()) << std::endl;
		std::cout << "The string content is: " << input->c_str() << std::endl;
		strings.emplace_back(std::move(input));
	}
	// Block until killed so shared-memory strings remain valid for other threadprocs.
	pause();
	return 0;
}
