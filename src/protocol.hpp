#pragma once

#include <cstdint>

#include <stdexcept>
#include <string>
#include <vector>

namespace ulab {

struct client_request {
	enum class kind {
		stdin_fd,
		stdout_fd,
		stderr_fd,
		cwd,
		env,
		args,
		signal,
	};
	kind type;
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
		struct {
			int signo;
		} signal;
	} payload[];

	std::vector<std::string> get_env() const {
		if (type != kind::env) {
			throw std::runtime_error("Invalid request type for get_env");
		}
		std::vector<std::string> result;
		size_t offset = 0;
		char const* const base = payload[0].env.vars;
		for (size_t i = 0; i < payload->env.num_vars; i++) {
			if (offset >= total_len - offsetof(client_request, payload[0].env.vars)) {
				throw std::runtime_error("Malformed env payload: offset exceeds total length");
			}
			const char* var = base + offset;
			result.emplace_back(var);
			offset += result.back().size() + 1;
		}
		return result;
	}

	std::vector<std::string> get_args() const {
		if (type != kind::args) {
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

struct server_notification {
	enum class kind {
		child_exit,
	};
	kind type;
	union {
		struct {
			pid_t tid;
			int exit_status;
		} child_exit;
	};
};

} // namespace ulab
