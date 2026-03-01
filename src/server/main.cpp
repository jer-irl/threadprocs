#include "server.hpp"

#include <sys/un.h>

#include <malloc.h>

#include <csignal>
#include <filesystem>
#include <iostream>

namespace ulab {
namespace {
std::filesystem::path g_socket_path;
} // namespace
} // namespace ulab


int main(int argc, char *argv[]) {

	mallopt(M_MMAP_THRESHOLD, 0);

	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <socket_path>" << std::endl;
		return 1;
	}

	std::cout << "Server PID " << getpid() << std::endl;

	char const* const socket_path = argv[1];
	ulab::g_socket_path = socket_path;

	struct stat statbuf;
	int rc = fstatat(AT_FDCWD, socket_path, &statbuf, 0);
	if (rc == 0) {
		std::cerr << "Error: file already exists at path '" << socket_path << "'" << std::endl;
		return 1;
	} else if (errno != ENOENT) {
		std::cerr << "Error checking socket path: " << strerror(errno) << std::endl;
		return 1;
	}

	int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sockfd == -1) {
		std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
		return 1;
	}

	sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = {0},
	};
	strncpy(&addr.sun_path[0], socket_path, sizeof(addr.sun_path));
	if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
		std::cerr << "Error: socket path is too long" << std::endl;
		return 1;
	}

	// TODO security on socket

	rc = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc == -1) {
		std::cerr << "Error binding socket: " << strerror(errno) << std::endl;
		return 1;
	}
	rc = listen(sockfd, 1);
	if (rc == -1) {
		std::cerr << "Error listening on socket: " << strerror(errno) << std::endl;
		return 1;
	}

	ulab::Server s{sockfd};

	constexpr auto cleanup = +[] {
		std::filesystem::remove(ulab::g_socket_path);
	};
	constexpr auto cleanup_sig = +[](int signum) {
		std::cout << "Received signal " << signum << ", cleaning up and exiting" << std::endl;
		cleanup();
		std::exit(0);
	};
	std::atexit(cleanup);
	std::set_terminate(cleanup);
	std::signal(SIGINT, cleanup_sig);
	std::signal(SIGQUIT, cleanup_sig);

	s.run();

	cleanup();

	return 0;
}