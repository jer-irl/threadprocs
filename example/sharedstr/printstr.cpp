#include <iostream>

int main() {
	while (true) {
		void* ptr = nullptr;
		std::cout << "Enter a hex pointer to a null-terminated string in virtual address space: " << std::endl;
		if (!(std::cin >> ptr >> std::dec)) {
			break;
		}
		std::cout << "The string content at address " << reinterpret_cast<void*>(ptr) << " is: " << reinterpret_cast<const char*>(ptr) << std::endl;
	}
}