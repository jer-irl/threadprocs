#include "server.hpp"

#include "protocol.hpp"
#include "server/util.hpp"
#include "tproc.h"
#include "trampoline_fwd.hpp"

#include <spdlog/spdlog.h>

#include <linux/sched.h>

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>

#include <format>

namespace ulab {

namespace {

int get_fd_from_cmsg(msghdr& msg) {
	for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			int fd = *(int*)CMSG_DATA(cmsg);
			return fd;
		}
	}
	return -1;
}

// Build the synthetic initial stack that ld-linux.so (or a static-PIE _start) expects.
// Returns a pointer to the bottom of the structured data (the initial SP value).
void* build_synthetic_stack(
	char* stack_top,
	const std::vector<std::string>& args,
	const std::vector<std::string>& env,
	const LoadedElf& target,
	const LoadedElf* interp,           // nullptr if no interpreter (static binary)
	void* registry_page)
{
	char* cursor = stack_top;

	// --- String data and random bytes (placed at high addresses) ---

	// 16 random bytes for AT_RANDOM
	cursor -= 16;
	getrandom(cursor, 16, 0);
	void* at_random = cursor;

	// Environment strings (packed, null-terminated)
	std::vector<char*> env_addrs(env.size());
	for (int i = (int)env.size() - 1; i >= 0; i--) {
		size_t len = env[i].size() + 1;
		cursor -= len;
		std::memcpy(cursor, env[i].c_str(), len);
		env_addrs[i] = cursor;
	}

	// Argument strings
	std::vector<char*> arg_addrs(args.size());
	for (int i = (int)args.size() - 1; i >= 0; i--) {
		size_t len = args[i].size() + 1;
		cursor -= len;
		std::memcpy(cursor, args[i].c_str(), len);
		arg_addrs[i] = cursor;
	}

	// --- Structured section (placed at lower addresses) ---

	// Count auxv entries
	int auxv_count = 8; // PHDR, PHNUM, PHENT, PAGESZ, BASE, ENTRY, RANDOM, TPROC_REGISTRY
	unsigned long vdso = getauxval(AT_SYSINFO_EHDR);
	if (vdso) auxv_count++;
	auxv_count++; // AT_NULL

	size_t struct_words =
		1                     // argc
		+ args.size() + 1     // argv pointers + NULL
		+ env.size() + 1      // envp pointers + NULL
		+ (size_t)auxv_count * 2;  // auxv pairs (key, value)

	size_t struct_bytes = struct_words * sizeof(unsigned long);

	// Align cursor down to 16 bytes, then subtract structured section
	cursor = reinterpret_cast<char*>(
		(reinterpret_cast<uintptr_t>(cursor) - struct_bytes) & ~uintptr_t(15));

	auto* w = reinterpret_cast<unsigned long*>(cursor);

	// argc
	*w++ = args.size();

	// argv pointers
	for (size_t i = 0; i < args.size(); i++)
		*w++ = reinterpret_cast<unsigned long>(arg_addrs[i]);
	*w++ = 0; // NULL

	// envp pointers
	for (size_t i = 0; i < env.size(); i++)
		*w++ = reinterpret_cast<unsigned long>(env_addrs[i]);
	*w++ = 0; // NULL

	// Auxiliary vector
	auto auxv = [&](unsigned long key, unsigned long val) {
		*w++ = key; *w++ = val;
	};

	auxv(AT_PHDR,   reinterpret_cast<unsigned long>(target.phdr));
	auxv(AT_PHNUM,  target.phnum);
	auxv(AT_PHENT,  target.phentsize);
	auxv(AT_PAGESZ, static_cast<unsigned long>(sysconf(_SC_PAGESIZE)));
	auxv(AT_BASE,   interp ? reinterpret_cast<unsigned long>(interp->base) : 0);
	auxv(AT_ENTRY,  reinterpret_cast<unsigned long>(target.entry));
	auxv(AT_RANDOM, reinterpret_cast<unsigned long>(at_random));
	auxv(AT_TPROC_REGISTRY, reinterpret_cast<unsigned long>(registry_page));
	if (vdso)
		auxv(AT_SYSINFO_EHDR, vdso);
	auxv(AT_NULL, 0);

	return cursor;
}

} // namespace


Server::Server(int sockfd) : sockfd{sockfd}, ring{} {
	int rc = io_uring_queue_init(16, &ring, 0);
	if (rc != 0) {
		throw std::runtime_error(std::format("Failed to initialize io_uring: {}", strerror(-rc)));
	}
	registry_page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (registry_page == MAP_FAILED) {
		io_uring_queue_exit(&ring);
		throw std::runtime_error("Failed to allocate registry page");
	}
}

void Server::spawn_client(LauncherInfo& client) {
	// --- Step 1: Load the target ELF binary ---
	auto target_result = LoadedElf::load_from_path(client.args[0]);
	if (!target_result) {
		spdlog::error("Error loading target ELF '{}': {}", client.args[0], strerror(-target_result.error()));
		return;
	}
	auto& tr = target_result.value();
	spdlog::debug("Loaded target: base={} entry={} phdr={} interp='{}'",
		tr.base, tr.entry, tr.phdr, tr.interp);

	// --- Step 2: Load the interpreter (ld-linux.so) if the target has PT_INTERP ---

	std::optional<LoadedElf> interp{std::nullopt};
	if (!tr.interp.empty()) {
		auto exp_interp = LoadedElf::load_from_path(tr.interp);
		if (!exp_interp) {
			spdlog::error("Error loading interpreter '{}': {}", tr.interp, strerror(-exp_interp.error()));
			return;
		}
		interp = std::move(exp_interp.value());
		spdlog::debug("Loaded interpreter: base={} entry={}", interp->base, interp->entry);
	}


	// --- Step 3: Allocate process stack and build synthetic stack layout ---
	auto const pstack_size = 8 * 1024 * 1024; // 8 MiB
	RaiiMunmap process_stack = std::span{static_cast<std::byte*>(mmap(nullptr, pstack_size,
							PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0)),
							pstack_size};
	if (process_stack->data() == MAP_FAILED) {
		spdlog::error("Error allocating process stack: {}", strerror(errno));
		return;
	}

	auto* stack_top = process_stack->data() + process_stack->size();

	// Force glibc malloc to use mmap instead of brk for all allocations.
	// With CLONE_VM, all threadprocs share a single mm_struct and therefore
	// a single brk region. Each threadproc's independent libc tracks its own
	// __curbrk, so competing sbrk() calls corrupt or unmap each other's heap.
	// Setting the mmap threshold to 0 avoids brk entirely.
	client.env.emplace_back("MALLOC_MMAP_THRESHOLD_=0");


	void* synthetic_sp = build_synthetic_stack(
		reinterpret_cast<char*>(stack_top), client.args, client.env, tr, interp.transform([](auto& interp) { return &interp; }).value_or(nullptr), registry_page);

	spdlog::debug("Synthetic SP: {} stack range: {} - {}", synthetic_sp, static_cast<void*>(process_stack->data()), static_cast<void*>(stack_top));

	// --- Step 4: Allocate a small stack for clone3 (child uses it briefly before switching) ---
	auto const c3stack_size = 64 * 1024; // 64 KiB
	RaiiMunmap clone3_stack = std::span{static_cast<std::byte*>(mmap(nullptr, c3stack_size,
							PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0)),
							c3stack_size};
	if (clone3_stack->data() == MAP_FAILED) {
		spdlog::error("Error allocating clone3 stack: {}", strerror(errno));
		return;
	}

	auto& ei = client.exec_info.emplace(LauncherInfo::ExecInfo{
		.clone3_stack = std::move(clone3_stack),
		.process_stack = std::move(process_stack),
		.target = std::move(tr),
		.interp = std::move(interp),
	});


	// --- Step 5: Set up clone3 args ---
	clone_args args{};
	args.flags = CLONE_VM | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID
				| CLONE_CLEAR_SIGHAND | CLONE_PIDFD;
	// NOT: CLONE_SETTLS (ld-linux.so initializes TLS), CLONE_FILES, CLONE_FS,
	//      CLONE_THREAD, CLONE_PARENT, CLONE_VFORK
	args.child_tid = reinterpret_cast<uint64_t>(&ei.tid_in_child);
	args.parent_tid = reinterpret_cast<uint64_t>(&ei.tid_in_parent);
	args.pidfd = reinterpret_cast<uint64_t>(&ei.pidfd);
	args.exit_signal = SIGCHLD;
	args.stack = reinterpret_cast<uint64_t>(ei.clone3_stack->data());
	args.stack_size = ei.clone3_stack->size();
	args.set_tid = 0;
	args.set_tid_size = 0;
	args.cgroup = 0;
	args.tls = 0;

	// --- Step 6: Clone + trampoline ---
	void* entry_point = ei.interp.has_value() ? ei.interp->entry : ei.target.entry;
	std::string cwd_str = client.cwd.string();
	long child_tid = child_clone3_and_exec(
		&args, sizeof(args),
		synthetic_sp,
		entry_point,
		client.stdin_fd,
		client.stdout_fd,
		client.stderr_fd,
		cwd_str.c_str());

	if (child_tid < 0) {
		spdlog::error("Error in clone3: {}", strerror((int)-child_tid));
		client.exec_info.reset();
		return;
	}

	// --- Parent continues ---
	ei.tid_in_parent = (pid_t)child_tid;
	close(client.stderr_fd);
	close(client.stdout_fd);
	close(client.stdin_fd);
	client.status = LauncherInfo::Status::executing;
	spdlog::info("Spawned client process with TID {}", child_tid);
	request_waitid(client);
}

int Server::request_accept() {
	io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, sockfd, nullptr, nullptr, 0);
	RingRequestInfo& accept_request_info = pending_requests.emplace_back();
	accept_request_info.type = RingRequestInfo::Kind::accept;
	io_uring_sqe_set_data(sqe, &accept_request_info);
	return io_uring_submit(&ring);
}

int Server::on_accept_cmpl(RingRequestInfo const& info, int rc) {
	(void) info;
	if (rc < 0) {
		spdlog::error("Error accepting connection: {}", strerror(-rc));
		return 1;
	}
	spdlog::info("Accepted connection");

	// TODO security: check peer credentials, permissions on socket, etc.

	std::unique_ptr<LauncherInfo>& client = clients.emplace_back(std::make_unique<LauncherInfo>());
	client->status = LauncherInfo::Status::connected;
	client->conn_fd = rc;

	// Listen for messages
	request_recvmsg(*client);

	// Continue accepting more connections
	request_accept();

	return 0;
}

int Server::request_recvmsg(LauncherInfo& client) {
	io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	RingRequestInfo& recvmsg_request_info = pending_requests.emplace_back();
	recvmsg_request_info.type = RingRequestInfo::Kind::recvmsg;
	auto& recvmsg_info = recvmsg_request_info.info.recvmsg;
	recvmsg_info.client = client;
	recvmsg_info.msg.msg_iov = &recvmsg_info.iov;
	recvmsg_info.msg.msg_iovlen = 1;
	recvmsg_info.iov.iov_base = recvmsg_info.buf;
	recvmsg_info.iov.iov_len = sizeof(recvmsg_info.buf);
	recvmsg_info.msg.msg_control = recvmsg_info.control_un.control_buf;
	recvmsg_info.msg.msg_controllen = sizeof(recvmsg_info.control_un.control_buf);
	recvmsg_info.msg.msg_name = nullptr;
	recvmsg_info.msg.msg_namelen = 0;
	io_uring_prep_recvmsg(sqe, client.conn_fd, &recvmsg_info.msg, MSG_CMSG_CLOEXEC);
	io_uring_sqe_set_data(sqe, &recvmsg_request_info);
	return io_uring_submit(&ring);
}

int Server::on_recvmsg_cmpl(RingRequestInfo& req_info, int rc) {
	LauncherInfo& client = req_info.info.recvmsg.client;

	if (rc < 0) {
		if ((rc == -EBADF || rc == -ECONNRESET)) {
			spdlog::info("Client disconnected");
			if (client.status == LauncherInfo::Status::finished) {
				spdlog::info("Client process already finished, just cleaning up connection");
			} else {
				spdlog::info("Client process not finished, but connection was closed. Marking client as closed and cleaning up.");
			}
			client.status = LauncherInfo::Status::closed;
			return 0;
		}
		spdlog::error("Error receiving message: {}", strerror(-rc));
		return 1;
	}

	if (rc == 0) {
		// Peer closed the connection (orderly shutdown)
		if (client.status != LauncherInfo::Status::finished) {
			spdlog::warn("Client connection closed before process finished");
		}
		client.status = LauncherInfo::Status::closed;
		return 0;
	}

	spdlog::info("Received message");

	msghdr& msg = req_info.info.recvmsg.msg;

	if (msg.msg_iovlen != 1) {
		spdlog::error("Error: expected exactly 1 iovec in message");
		return 1;
	}

	if (rc < (int)sizeof(ClientRequest)) {
		spdlog::error("Error: message too short to contain client_request");
		return 1;
	}

	if (msg.msg_iov[0].iov_base == nullptr) {
		spdlog::error("Error: message iovec base is null");
		return 1;
	}

	if (reinterpret_cast<std::uintptr_t>(msg.msg_iov[0].iov_base) % alignof(ClientRequest) != 0) {
		spdlog::error("Error: message iovec base is not properly aligned for client_request");
		return 1;
	}

	ClientRequest* request = reinterpret_cast<ClientRequest*>(msg.msg_iov[0].iov_base);

	switch (request->type) {
	case ClientRequest::Kind::stdin_fd:
		client.stdin_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
		break;
	case ClientRequest::Kind::stdout_fd:
		client.stdout_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
		break;
	case ClientRequest::Kind::stderr_fd:
		client.stderr_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
		break;
	case ClientRequest::Kind::cwd:
		client.cwd = std::filesystem::path{request->payload[0].cwd};
		break;
	case ClientRequest::Kind::env:
		client.env = request->get_env();
		break;
	case ClientRequest::Kind::args:
		client.args = request->get_args();
		break;
	case ClientRequest::Kind::signal: {
		spdlog::info("Received signal notification from client: signo {}", request->payload[0].signal.signo);
		if (client.exec_info == std::nullopt || client.status != LauncherInfo::Status::executing) {
			spdlog::warn("Warning: received signal notification but client process is not executing. Ignoring signal.");
			break;
		}
		int rc = kill(client.exec_info->tid_in_parent, request->payload[0].signal.signo);
		if (rc == -1) {
			spdlog::error("Error forwarding signal to child process: {}", strerror(errno));
		} else {
			spdlog::info("Forwarded signal {} to child process with TID {}", request->payload[0].signal.signo, client.exec_info->tid_in_parent);
		}
		break;
	}
	default:
		spdlog::error("Error: unknown client request type");
		return 1;
	}

	if (client.ready_to_exec() && client.status == LauncherInfo::Status::connected) {
		std::string joined{client.args.empty() ? "" : client.args[0]};
		for (size_t i = 1; i < client.args.size(); i++) {
			joined += " " + client.args[i];
		}
		spdlog::info("Client is ready to execute program with args: {}", joined);
		spawn_client(client);
	}

	request_recvmsg(client);

	return 0;
}

int Server::request_waitid(LauncherInfo& client) {
	io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	RingRequestInfo& req_info = pending_requests.emplace_back();
	req_info.type = RingRequestInfo::Kind::waitid;
	auto& wait_info = req_info.info.waitid;
	wait_info.client = client;
	io_uring_prep_waitid(sqe, P_PIDFD, client.exec_info->pidfd.value(), &wait_info.siginfo, WEXITED, 0);
	io_uring_sqe_set_data(sqe, &req_info);
	return io_uring_submit(&ring);
}

int Server::on_waitid_cmpl(RingRequestInfo& req_info, int rc) {
	if (rc != 0) {
		spdlog::error("Error waiting for child process: {} child pid: {}", strerror(-rc), req_info.info.waitid.client.get().exec_info->tid_in_parent);
		return 1;
	}

	LauncherInfo& client = req_info.info.waitid.client;
	spdlog::info("Client process with TID {} exited with status {} and code {}", client.exec_info->tid_in_parent, req_info.info.waitid.siginfo.si_status, req_info.info.waitid.siginfo.si_code);

	if (req_info.info.waitid.siginfo.si_code != CLD_EXITED) {
		spdlog::warn("Warning: child did not exit normally, si_code: {}", req_info.info.waitid.siginfo.si_code);
	}

	client.status = LauncherInfo::Status::finished;

	ServerNotification notification{ServerNotification::Kind::child_exit, {client.exec_info->tid_in_parent, req_info.info.waitid.siginfo.si_status}};
	int sent = send(client.conn_fd, &notification, sizeof(notification), 0); // notify client that process has finished
	if (sent == -1) {
		spdlog::error("Error sending notification to client: {}", strerror(errno));
	}
	if (sent != sizeof(notification)) {
		spdlog::error("Error: sent {} bytes, expected to send {} bytes", sent, sizeof(notification));
	}

	// Free resources now that the child has exited
	client.exec_info.reset();

	spdlog::info("Notified client of process exit");

	return 0;
}

} // namespace ulab
