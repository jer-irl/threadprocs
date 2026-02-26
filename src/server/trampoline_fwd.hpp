#pragma once

#include <cstddef>

extern "C" long child_clone3_and_exec(
	struct clone_args *args,
	size_t args_size,
	void *synthetic_sp,
	void *entry_point,
	int stdin_fd,
	int stdout_fd,
	int stderr_fd,
	const char *cwd_path
);
