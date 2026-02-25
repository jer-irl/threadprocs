#include "elf_loader.hpp"
#include "protocol.hpp"

#include <exception>
#include <liburing.h>
#include <liburing/io_uring.h>

#include <bits/types/struct_iovec.h>
#include <elf.h>
#include <linux/sched.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

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

namespace ulab {

enum class ring_request_type {
	accept,
	recvmsg,
	waitid,
};

template<typename T>
struct intrusive_list_node {

	intrusive_list_node() : value{T{}} {}
	template<typename... Args>
	intrusive_list_node(Args&&... args) : value{std::forward<Args>(args)...} {}

	std::optional<T> value{std::nullopt};
	intrusive_list_node* next{nullptr};
	intrusive_list_node* prev{nullptr};
};

template<typename T>
class intrusive_list {
public:
	static_assert(offsetof(intrusive_list_node<T>, value) == 0, "intrusive_list_node<T>::value must be at offset 0");

	intrusive_list() {
		sentinel.next = &sentinel;
		sentinel.prev = &sentinel;
	}

	template<typename... Args>
	T& emplace_back(Args&&... args) {
		auto* new_node = new intrusive_list_node<T>{std::forward<Args>(args)...};
		new_node->next = &sentinel;
		new_node->prev = sentinel.prev;
		sentinel.prev->next = new_node;
		sentinel.prev = new_node;
		return new_node->value.value();
	}

	void erase(T& item) {
		auto& opt = reinterpret_cast<std::optional<T>&>(item);
		auto* intrusive_node = reinterpret_cast<intrusive_list_node<T>*>(&opt);
		intrusive_node->prev->next = intrusive_node->next;
		intrusive_node->next->prev = intrusive_node->prev;
		delete intrusive_node;
	}

private:
	intrusive_list_node<T> sentinel{};
};

struct client_info;

struct ring_request_info {
	ring_request_type type;
	union {
		struct {
		} accept;
		struct {
			std::reference_wrapper<client_info> client;
			msghdr msg;
			iovec iov;
			char buf[4096];
			union {
				struct cmsghdr align[0];
				char control_buf[CMSG_SPACE(sizeof(int))];
			} control_un;
		} recvmsg;
		struct {
			std::reference_wrapper<client_info> client;
			siginfo_t siginfo;
		} waitid;
	} info;
};

struct client_info {
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

	struct exec_info {
		pid_t tid_in_child; // also futex
		pid_t tid_in_parent;
		int pidfd;
		void* clone3_stack;       // small stack for clone3 child to trampoline on
		std::size_t clone3_stack_size;
		void* process_stack;      // main stack for the loaded program
		std::size_t process_stack_size;
		loaded_elf target;
		loaded_elf interp;        // only valid if target has PT_INTERP
	};

	std::optional<exec_info> exec_info;

	bool ready_to_exec() const {
		return conn_fd != -1 && stdout_fd != -1 && stderr_fd != -1 && stdin_fd != -1 && !args.empty() && !env.empty() && !cwd.empty();
	}
};

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
	const loaded_elf& target,
	const loaded_elf* interp)  // nullptr if no interpreter (static binary)
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
	int auxv_count = 7; // PHDR, PHNUM, PHENT, PAGESZ, BASE, ENTRY, RANDOM
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
	if (vdso)
		auxv(AT_SYSINFO_EHDR, vdso);
	auxv(AT_NULL, 0);

	return cursor;
}

class server {
public:
	server(int sockfd) : sockfd{sockfd}, ring{} {
		int rc = io_uring_queue_init(16, &ring, 0);
		if (rc != 0) {
			std::cerr << "Error initializing io_uring: " << strerror(-rc) << std::endl;
			throw std::runtime_error("Failed to initialize io_uring");
		}
	}

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
			switch (request_info->type) {
			case ring_request_type::accept:
				rc = on_accept_cmpl(*request_info, cqe->res);
				break;
			case ring_request_type::recvmsg:
				rc = on_recvmsg_cmpl(*request_info, cqe->res);
				break;
			case ring_request_type::waitid:
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
	int request_accept() {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		io_uring_prep_accept(sqe, sockfd, nullptr, nullptr, 0);
		ring_request_info& accept_request_info = pending_requests.emplace_back();
		accept_request_info.type = ring_request_type::accept;
		io_uring_sqe_set_data(sqe, &accept_request_info);
		return io_uring_submit(&ring);
	}

	int on_accept_cmpl(ring_request_info const& info, int rc) {
		(void) info;
		if (rc < 0) {
			std::cerr << "Error accepting connection: " << strerror(-rc) << std::endl;
			return 1;
		}
		std::cout << "Accepted connection" << std::endl;

		// TODO security: check peer credentials, permissions on socket, etc.

		std::unique_ptr<client_info>& client = clients.emplace_back(std::make_unique<client_info>());
		client->status = client_info::status::connected;
		client->conn_fd = rc;

		// Listen for messages
		request_recvmsg(*client);

		// Continue accepting more connections
		request_accept();

		return 0;
	}

	int request_recvmsg(client_info& client) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		ring_request_info& recvmsg_request_info = pending_requests.emplace_back();
		recvmsg_request_info.type = ring_request_type::recvmsg;
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

	int on_recvmsg_cmpl(ring_request_info& req_info, int rc) {
		client_info& client = req_info.info.recvmsg.client;

		if (rc < 0) {
			if ((rc == -EBADF || rc == -ECONNRESET)) {
				std::cerr << "Client disconnected" << std::endl;
				if (client.status == client_info::status::finished) {
					std::cerr << "Client process already finished, just cleaning up connection" << std::endl;
				} else {
					std::cerr << "Client process not finished, but connection was closed. Marking client as closed and cleaning up." << std::endl;
				}
				client.status = client_info::status::closed;
				close(client.conn_fd);
				return 0;
			}
			std::cerr << "Error receiving message: " << strerror(-rc) << std::endl;
			return 1;
		}

		if (rc == 0) {
			// Peer closed the connection (orderly shutdown)
			if (client.status != client_info::status::finished) {
				std::cerr << "Client connection closed before process finished" << std::endl;
			}
			client.status = client_info::status::closed;
			return 0;
		}

		std::cout << "Received message" << std::endl;

		msghdr& msg = req_info.info.recvmsg.msg;

		if (msg.msg_iovlen != 1) {
			std::cerr << "Error: expected exactly 1 iovec in message" << std::endl;
			return 1;
		}

		if (rc < (int)sizeof(client_request)) {
			std::cerr << "Error: message too short to contain client_request" << std::endl;
			return 1;
		}

		if (msg.msg_iov[0].iov_base == nullptr) {
			std::cerr << "Error: message iovec base is null" << std::endl;
			return 1;
		}

		if (reinterpret_cast<std::uintptr_t>(msg.msg_iov[0].iov_base) % alignof(client_request) != 0) {
			std::cerr << "Error: message iovec base is not properly aligned for client_request" << std::endl;
			return 1;
		}

		client_request* request = reinterpret_cast<client_request*>(msg.msg_iov[0].iov_base);

		switch (request->type) {
		case client_request::kind::stdin_fd:
			client.stdin_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
			break;
		case client_request::kind::stdout_fd:
			client.stdout_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
			break;
		case client_request::kind::stderr_fd:
			client.stderr_fd = get_fd_from_cmsg(req_info.info.recvmsg.msg);
			break;
		case client_request::kind::cwd:
			client.cwd = std::filesystem::path{request->payload[0].cwd};
			break;
		case client_request::kind::env:
			client.env = request->get_env();
			break;
		case client_request::kind::args:
			client.args = request->get_args();
			break;
		case client_request::kind::signal: {
			std::cout << "Received signal notification from client: signo " << request->payload[0].signal.signo << std::endl;
			if (client.exec_info == std::nullopt || client.status != client_info::status::executing) {
				std::cerr << "Warning: received signal notification but client process is not executing. Ignoring signal." << std::endl;
				break;
			}
			int rc = kill(client.exec_info->tid_in_parent, request->payload[0].signal.signo);
			if (rc == -1) {
				std::cerr << "Error forwarding signal to child process: " << strerror(errno) << std::endl;
			} else {
				std::cout << "Forwarded signal " << request->payload[0].signal.signo << " to child process with TID " << client.exec_info->tid_in_parent << std::endl;
			}
			break;
		}
		default:
			std::cerr << "Error: unknown client request type" << std::endl;
			return 1;
		}

		if (client.ready_to_exec() && client.status == client_info::status::connected) {
			std::cout << "Client is ready to execute program with args: ";
			for (const auto& arg : client.args) {
				std::cout << arg << " ";
			}
			std::cout << std::endl;
			spawn_client(client);
		}

		request_recvmsg(client);

		return 0;
	}

	void spawn_client(client_info& client) {
		client.exec_info.emplace();
		auto& ei = *client.exec_info;

		// --- Step 1: Load the target ELF binary ---
		int rc = load_elf(client.args[0].c_str(), ei.target);
		if (rc != 0) {
			std::cerr << "Error loading target ELF '" << client.args[0] << "': " << strerror(-rc) << std::endl;
			client.exec_info.reset();
			return;
		}
		std::cerr << "Loaded target: base=" << ei.target.base
		          << " entry=" << ei.target.entry
		          << " phdr=" << ei.target.phdr
		          << " interp='" << ei.target.interp << "'" << std::endl;

		// --- Step 2: Load the interpreter (ld-linux.so) if the target has PT_INTERP ---
		void* entry_point = ei.target.entry;
		loaded_elf* interp_ptr = nullptr;

		if (!ei.target.interp.empty()) {
			rc = load_elf(ei.target.interp.c_str(), ei.interp);
			if (rc != 0) {
				std::cerr << "Error loading interpreter '" << ei.target.interp << "': " << strerror(-rc) << std::endl;
				munmap(ei.target.map, ei.target.map_len);
				client.exec_info.reset();
				return;
			}
			entry_point = ei.interp.entry;
			interp_ptr = &ei.interp;
			std::cerr << "Loaded interpreter: base=" << ei.interp.base
			          << " entry=" << ei.interp.entry << std::endl;
		}

		// --- Step 3: Allocate process stack and build synthetic stack layout ---
		ei.process_stack_size = 8 * 1024 * 1024; // 8 MiB
		ei.process_stack = mmap(nullptr, ei.process_stack_size,
		                        PROT_READ | PROT_WRITE,
		                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
		if (ei.process_stack == MAP_FAILED) {
			std::cerr << "Error allocating process stack: " << strerror(errno) << std::endl;
			client.exec_info.reset();
			return;
		}

		char* stack_top = static_cast<char*>(ei.process_stack) + ei.process_stack_size;

		// Force glibc malloc to use mmap instead of brk for all allocations.
		// With CLONE_VM, all threadprocs share a single mm_struct and therefore
		// a single brk region. Each threadproc's independent libc tracks its own
		// __curbrk, so competing sbrk() calls corrupt or unmap each other's heap.
		// Setting the mmap threshold to 0 avoids brk entirely.
		client.env.emplace_back("MALLOC_MMAP_THRESHOLD_=0");

		void* synthetic_sp = build_synthetic_stack(
			stack_top, client.args, client.env, ei.target, interp_ptr);

		std::cerr << "Synthetic SP: " << synthetic_sp
		          << " stack range: " << ei.process_stack << " - " << (void*)stack_top << std::endl;

		// --- Step 4: Allocate a small stack for clone3 (child uses it briefly before switching) ---
		ei.clone3_stack_size = 64 * 1024; // 64 KiB
		ei.clone3_stack = mmap(nullptr, ei.clone3_stack_size,
		                       PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
		if (ei.clone3_stack == MAP_FAILED) {
			std::cerr << "Error allocating clone3 stack: " << strerror(errno) << std::endl;
			munmap(ei.process_stack, ei.process_stack_size);
			client.exec_info.reset();
			return;
		}

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
		args.stack = reinterpret_cast<uint64_t>(ei.clone3_stack);
		args.stack_size = ei.clone3_stack_size;
		args.set_tid = 0;
		args.set_tid_size = 0;
		args.cgroup = 0;
		args.tls = 0;

		// --- Step 6: Clone + trampoline ---
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
			std::cerr << "Error in clone3: " << strerror((int)-child_tid) << std::endl;
			munmap(ei.clone3_stack, ei.clone3_stack_size);
			munmap(ei.process_stack, ei.process_stack_size);
			client.exec_info.reset();
			return;
		}

		// --- Parent continues ---
		ei.tid_in_parent = (pid_t)child_tid;
		close(client.stderr_fd);
		close(client.stdout_fd);
		close(client.stdin_fd);
		client.status = client_info::status::executing;
		std::cout << "Spawned client process with TID " << child_tid << std::endl;
		request_waitid(client);
	}

	int request_waitid(client_info& client) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		ring_request_info& req_info = pending_requests.emplace_back();
		req_info.type = ring_request_type::waitid;
		auto& wait_info = req_info.info.waitid;
		wait_info.client = client;
		io_uring_prep_waitid(sqe, P_PIDFD, client.exec_info->pidfd, &wait_info.siginfo, WEXITED, 0);
		io_uring_sqe_set_data(sqe, &req_info);
		return io_uring_submit(&ring);
	}
	int on_waitid_cmpl(ring_request_info& req_info, int rc) {
		if (rc != 0) {
			std::cerr << "Error waiting for child process: " << strerror(-rc) << " child pid: " << req_info.info.waitid.client.get().exec_info->tid_in_parent << std::endl;
			return 1;
		}

		client_info& client = req_info.info.waitid.client;
		std::cout << "Client process with TID " << client.exec_info->tid_in_parent << " exited with status " << req_info.info.waitid.siginfo.si_status << " and code " << req_info.info.waitid.siginfo.si_code << std::endl;

		if (req_info.info.waitid.siginfo.si_code != CLD_EXITED) {
			std::cerr << "Warning: child did not exit normally, si_code: " << req_info.info.waitid.siginfo.si_code << std::endl;
		}

		client.status = client_info::status::finished;

		server_notification notification{server_notification::kind::child_exit, {client.exec_info->tid_in_parent, req_info.info.waitid.siginfo.si_status}};
		int sent = send(client.conn_fd, &notification, sizeof(notification), 0); // notify client that process has finished
		if (sent == -1) {
			std::cerr << "Error sending notification to client: " << strerror(errno) << std::endl;
		}
		if (sent != sizeof(notification)) {
			std::cerr << "Error: sent " << sent << " bytes, expected to send " << sizeof(notification) << " bytes" << std::endl;
		}

		close(client.conn_fd);
		close(client.exec_info->pidfd);

		// Free mmap'd resources now that the child has exited
		auto& ei = *client.exec_info;
		munmap(ei.clone3_stack, ei.clone3_stack_size);
		munmap(ei.process_stack, ei.process_stack_size);
		munmap(ei.target.map, ei.target.map_len);
		if (ei.interp.map)
			munmap(ei.interp.map, ei.interp.map_len);
		client.exec_info.reset();

		std::cout << "Notified client of process exit" << std::endl;

		return 0;
	}

	int sockfd;
	io_uring ring;
	intrusive_list<ring_request_info> pending_requests;
	std::vector<std::unique_ptr<client_info>> clients;
};

std::filesystem::path g_socket_path;

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

	ulab::server s{sockfd};

	constexpr auto cleanup = +[] {
		std::filesystem::remove(ulab::g_socket_path);
	};
	constexpr auto cleanup_sig = +[](int signum) {
		std::cerr << "Received signal " << signum << ", cleaning up and exiting" << std::endl;
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