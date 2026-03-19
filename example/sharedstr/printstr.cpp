#include <iostream>

int main() {
	while (true) {
		void* ptr = nullptr;
		std::cout << "Enter a hex pointer to a std::string in virtual address space: " << std::endl;
		if (!(std::cin >> ptr >> std::dec)) {
			break;
		}
		std::string* s = reinterpret_cast<std::string*>(ptr);
		std::cout << "The std::string at address " << reinterpret_cast<void*>(ptr) << " is: " << *s << std::endl;
	}
}