#pragma once

#include <cstdint>

#include <stdexcept>
#include <string>
#include <vector>

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
		struct {
			std::uint64_t num_vars;
			char vars[0];
		} env;
		struct {
			std::uint64_t argc;
			char argz[0];
		} args;
	} payload[];

	std::vector<std::string> get_env() const {
		if (type != client_request_type::env) {
			throw std::runtime_error("Invalid request type for get_env");
		}
		std::vector<std::string> result;
		size_t offset = 0;
		for (size_t i = 0; i < payload->env.num_vars; i++) {
			if (offset >= total_len - offsetof(client_request, payload[0].env.vars)) {
				throw std::runtime_error("Malformed env payload: offset exceeds total length");
			}
			const char* var = payload->env.vars + offset;
			result.emplace_back(var);
			offset += result.back().size() + 1;
		}
		return result;
	}

	std::vector<std::string> get_args() const {
		if (type != client_request_type::args) {
			throw std::runtime_error("Invalid request type for get_args");
		}
		std::vector<std::string> result;
		size_t offset = 0;
		for (size_t i = 0; i < payload->args.argc; i++) {
			if (offset >= total_len - offsetof(client_request, payload[0].args.argz)) {
				throw std::runtime_error("Malformed args payload: offset exceeds total length");
			}
			const char* arg = payload->args.argz + offset;
			result.emplace_back(arg);
			offset += result.back().size() + 1;
		}
		return result;
	}
};

} // namespace ulab
