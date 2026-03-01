#include "server.hpp"
#include "spdlog/common.h"

#include <spdlog/spdlog.h>

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

	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << "[-v] <socket_path>" << std::endl;
		return 1;
	}


	int first_arg = 1;
	if (std::string_view(argv[1]) == "-v") {
		spdlog::set_level(spdlog::level::debug);
		first_arg += 1;
	} else {
		spdlog::set_level(spdlog::level::warn);
	}

	spdlog::info("Server PID {}", getpid());

	char const* const socket_path = argv[first_arg];
	ulab::g_socket_path = socket_path;

	struct stat statbuf;
	int rc = fstatat(AT_FDCWD, socket_path, &statbuf, 0);
	if (rc == 0) {
		spdlog::error("Error: file already exists at path '{}'", socket_path);
		return 1;
	} else if (errno != ENOENT) {
		spdlog::error("Error checking socket path: {}", strerror(errno));
		return 1;
	}

	int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sockfd == -1) {
		spdlog::error("Error creating socket: {}", strerror(errno));
		return 1;
	}

	sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = {0},
	};
	strncpy(&addr.sun_path[0], socket_path, sizeof(addr.sun_path));
	if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
		spdlog::error("Error: socket path is too long");

		return 1;
	}

	// TODO security on socket

	rc = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc == -1) {
		spdlog::error("Error binding socket: {}", strerror(errno));
		return 1;
	}
	rc = listen(sockfd, 1);
	if (rc == -1) {
		spdlog::error("Error listening on socket: {}", strerror(errno));
		return 1;
	}

	ulab::Server s{sockfd};

	constexpr auto cleanup = +[] {
		std::filesystem::remove(ulab::g_socket_path);
	};
	constexpr auto cleanup_sig = +[](int signum) {
		spdlog::info("Received signal {}, cleaning up and exiting", signum);
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