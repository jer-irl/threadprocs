#pragma once

#include "elf_loader.hpp"
#include "tproc.h"
#include "trampoline_fwd.hpp"
#include "util.hpp"

#include <liburing.h>
#include <liburing/io_uring.h>

#include <linux/sched.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>

#include <malloc.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ulab {

struct LauncherInfo;

struct ring_request_info {
	enum class kind {
		accept,
		recvmsg,
		waitid,
	} type;
	union {
		struct {
		} accept;
		struct {
			std::reference_wrapper<LauncherInfo> client;
			msghdr msg;
			iovec iov;
			char buf[4096];
			union {
				struct cmsghdr align[0];
				char control_buf[CMSG_SPACE(sizeof(int))];
			} control_un;
		} recvmsg;
		struct {
			std::reference_wrapper<LauncherInfo> client;
			siginfo_t siginfo;
		} waitid;
	} info;
};

struct LauncherInfo {
	enum class status {
		connected,
		executing,
		finished,
		closed,
	} status;

	int conn_fd{-1};
	int stdin_fd{-1};
	int stdout_fd{-1};
	int stderr_fd{-1};
	std::filesystem::path cwd;
	std::vector<std::string> env;
	std::vector<std::string> args;

	struct ExecInfo {
		explicit ExecInfo(LoadedElf target) : target{std::move(target)} {}

		pid_t tid_in_child; // also futex
		pid_t tid_in_parent;
		int pidfd;
		void* clone3_stack;       // small stack for clone3 child to trampoline on
		std::size_t clone3_stack_size;
		void* process_stack;      // main stack for the loaded program
		std::size_t process_stack_size;
		LoadedElf target;
		std::optional<LoadedElf> interp;        // only valid if target has PT_INTERP
	};

	std::optional<ExecInfo> exec_info;

	bool ready_to_exec() const {
		return conn_fd != -1 && stdout_fd != -1 && stderr_fd != -1 && stdin_fd != -1 && !args.empty() && !env.empty() && !cwd.empty();
	}
};

class Server {
public:
	explicit Server(int sockfd);

	void run() {
		request_accept();

		while (true) {
			io_uring_cqe *cqe;
			int rc = io_uring_wait_cqe(&ring, &cqe);
			if (rc != 0) {
				if (rc == -EINTR) {
					continue; // retry if interrupted by signal
				}
				std::cerr << "Error waiting for completion: " << strerror(-rc) << std::endl;
				throw std::runtime_error("Failed to wait for completion");
			}

			ring_request_info* request_info = (ring_request_info*)cqe->user_data;
			using rk = ring_request_info::kind;
			switch (request_info->type) {
			case rk::accept:
				rc = on_accept_cmpl(*request_info, cqe->res);
				break;
			case rk::recvmsg:
				rc = on_recvmsg_cmpl(*request_info, cqe->res);
				break;
			case rk::waitid:
				rc = on_waitid_cmpl(*request_info, cqe->res);
				break;
			}
			pending_requests.erase(*request_info);
			io_uring_cqe_seen(&ring, cqe);

			if (rc != 0) {
				std::cerr << "Error handling completion: " << strerror(-rc) << std::endl;
				throw std::runtime_error("Failed to handle completion");
			}
		}
	}

private:
	void spawn_client(LauncherInfo& client);

	int request_accept();
	int on_accept_cmpl(ring_request_info const& info, int rc);

	int request_recvmsg(LauncherInfo& client);
	int on_recvmsg_cmpl(ring_request_info& req_info, int rc);

	int request_waitid(LauncherInfo& client);
	int on_waitid_cmpl(ring_request_info& req_info, int rc);

	int sockfd;
	io_uring ring;
	IntrusiveList<ring_request_info> pending_requests;
	std::vector<std::unique_ptr<LauncherInfo>> clients;
	void* registry_page{nullptr};
};

} // namespace ulab
