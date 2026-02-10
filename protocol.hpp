#pragma once

#include <cstdint>

namespace ulab {

enum class client_request_type {
	stdin_fd,
	stdout_fd,
	stderr_fd,
	cwd,
	env,
	args,
};

struct client_request {
	client_request_type type;
	std::uint64_t total_len;
	union {
		char cwd[0];
		char env[0];
		char args[0];
	} payload[];
};

} // namespace ulab
