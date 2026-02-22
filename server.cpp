#include "protocol.hpp"

#include <liburing.h>
#include <liburing/io_uring.h>

#include <bits/types/struct_iovec.h>
#include <linux/sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
		std::byte* stack_area_low;
		std::byte* stack_area_high;
		std::size_t stack_size;
		std::byte* child_tls;
		std::size_t child_tls_size;
		std::uintptr_t parent_thread_pointer;
		std::uintptr_t child_thread_pointer;
		int parent_errno_before_clone;
		int parent_errno_after_clone;
		int child_errno_at_start;
		int child_errno_after_set;
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

std::uintptr_t get_current_thread_pointer() {
#if defined(__aarch64__)
	unsigned long tp = 0;
	asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
	return static_cast<std::uintptr_t>(tp);
#else
	return 0;
#endif
}

int create_private_tls_copy(std::byte*& tls_map, std::size_t& tls_map_size, std::uint64_t& tls_pointer) {
#if defined(__aarch64__)
	unsigned long current_tp = 0;
	asm volatile("mrs %0, tpidr_el0" : "=r"(current_tp));
	if (current_tp == 0) {
		std::cerr << "Error: parent TPIDR_EL0 is zero" << std::endl;
		return -1;
	}

	std::ifstream maps{"/proc/self/maps"};
	if (!maps) {
		std::cerr << "Error opening /proc/self/maps" << std::endl;
		return -1;
	}

	std::string line;
	std::uintptr_t region_start = 0;
	std::uintptr_t region_end = 0;
	while (std::getline(maps, line)) {
		unsigned long long start = 0;
		unsigned long long end = 0;
		if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) != 2) {
			continue;
		}
		if (current_tp >= start && current_tp < end) {
			region_start = static_cast<std::uintptr_t>(start);
			region_end = static_cast<std::uintptr_t>(end);
			break;
		}
	}

	if (region_start == 0 || region_end <= region_start) {
		std::cerr << "Error: failed to locate mapping containing TPIDR_EL0" << std::endl;
		return -1;
	}

	tls_map_size = region_end - region_start;
	tls_map = static_cast<std::byte*>(mmap(nullptr, tls_map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (tls_map == MAP_FAILED) {
		std::cerr << "Error allocating child TLS mapping: " << strerror(errno) << std::endl;
		tls_map = nullptr;
		tls_map_size = 0;
		return -1;
	}

	std::memcpy(tls_map, reinterpret_cast<void*>(region_start), tls_map_size);
	const std::uintptr_t tp_offset = static_cast<std::uintptr_t>(current_tp) - region_start;
	tls_pointer = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tls_map) + tp_offset);
	return 0;
#else
	(void)tls_map;
	(void)tls_map_size;
	(void)tls_pointer;
	std::cerr << "Error: private TLS setup is only implemented on AArch64" << std::endl;
	return -1;
#endif
}

[[noreturn]] void child_thread_main(client_info& client) {
	// TODO set up stdin, stdout, stderr, cwd, env, and args
	// execveat or something like that with the provided args and env
	// maybe use a custom syscall to pass the file descriptors since we already have them in memory and don't want to deal with the complexity of sending them over a socket again

	if (client.exec_info.has_value()) {
		client.exec_info->child_thread_pointer = get_current_thread_pointer();
		client.exec_info->child_errno_at_start = errno;
		errno = 3003;
		client.exec_info->child_errno_after_set = errno;
	}

	write(client.stdout_fd, "Hello from child thread!\n", 26);
	int rc;
	rc = dup3(client.stdin_fd, STDIN_FILENO, 0);
	if (rc != STDIN_FILENO) {
		std::cerr << "Error setting up stdin: " << strerror(errno) << std::endl;
		std::exit(1);
	}
	rc = dup3(client.stdout_fd, STDOUT_FILENO, 0);
	if (rc != STDOUT_FILENO) {
		std::cerr << "Error setting up stdout: " << strerror(errno) << std::endl;
		std::exit(1);
	}
	rc = dup3(client.stderr_fd, STDERR_FILENO, 0);
	if (rc != STDERR_FILENO) {
		std::cerr << "Error setting up stderr: " << strerror(errno) << std::endl;
		std::exit(1);
	}

	std::cout << "Child thread PID " << getpid() << std::endl;
	sleep(10);
	// std::exit(0);
	syscall(SYS_exit, 0);
	std::unreachable();
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
		std::cout << "Received message" << std::endl;

		msghdr& msg = req_info.info.recvmsg.msg;

		if (msg.msg_iovlen != 1) {
			std::cerr << "Error: expected exactly 1 iovec in message" << std::endl;
			return 1;
		}

		if (msg.msg_iov[0].iov_len < sizeof(client_request)) {
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
		default:
			std::cerr << "Error: unknown client request type" << std::endl;
			return 1;
		}

		if (client.ready_to_exec()) {
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

	// https://nullprogram.com/blog/2023/03/23/

	// 0x9000 <- start of copied stack frame (fp pointed-to)
	// 0x8000 <- new sp
	// 0x1000 <- new stack variable to syscall
	// stack size = 0x7000

	__attribute__((noinline)) static int spawn_client_impl(clone_args& args, client_info* client) {
		void* sp = nullptr;
		void* fp = nullptr;
		void* fp_deref = nullptr;
		size_t to_copy = 0;
		void* new_sp = nullptr;
		void* new_fp = nullptr;
		void* new_fp_deref = nullptr;
		size_t adjusted_stack_size = 0;
		void* args_stack = reinterpret_cast<void*>(args.stack);
		(void) args_stack;
		asm volatile("" : "+m" (client));
		#if defined(__x86_64__)
			#error "Not supported"
			asm volatile("mov %%rsp, %0\n\tmov %%rbp, %1" : "=r"(sp), "=r"(fp));
		#elif defined(__aarch64__)
			asm volatile("mov %0, sp\n\tmov %1, fp" : "=r"(sp), "=r"(fp));
		#else
			#error "Not supported"
			(void)sp; (void)fp;
		#endif

		fp_deref = *reinterpret_cast<void**>(fp);

		to_copy = reinterpret_cast<std::uintptr_t>(fp_deref) - reinterpret_cast<std::uintptr_t>(sp);
		new_sp = reinterpret_cast<void*>(args.stack + args.stack_size - to_copy);
		new_fp = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(new_sp) + (reinterpret_cast<std::uintptr_t>(fp) - reinterpret_cast<std::uintptr_t>(sp)));
		new_fp_deref = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(fp_deref) - reinterpret_cast<std::uintptr_t>(fp) + reinterpret_cast<std::uintptr_t>(new_fp));
		adjusted_stack_size = args.stack_size - to_copy;
		std::memcpy(new_sp, sp, to_copy);
		*reinterpret_cast<void**>(new_fp) = new_fp_deref;

		args.stack_size = adjusted_stack_size;
		void* new_stack = reinterpret_cast<void*>(args.stack);
		void* new_stack_area_high = reinterpret_cast<void*>(args.stack + args.stack_size);
		(void) new_stack;
		(void) new_stack_area_high;

		int rc = syscall(SYS_clone3, &args, sizeof(args));
		if (rc == -1) {
			std::cerr << "Error in clone3 syscall: " << strerror(errno) << std::endl;
			return -1;
		}
		void* c = client;
		(void) c;

		if (rc == 0) {
			// In child,
			child_thread_main(*client);
		}
		else {
			return rc;
		}
	}

	void spawn_client(client_info& client) {
		client.exec_info.emplace();
		client.exec_info->parent_thread_pointer = get_current_thread_pointer();
		client.exec_info->child_thread_pointer = 0;
		client.exec_info->parent_errno_before_clone = 1001;
		client.exec_info->parent_errno_after_clone = 0;
		client.exec_info->child_errno_at_start = 0;
		client.exec_info->child_errno_after_set = 0;
		errno = client.exec_info->parent_errno_before_clone;
		clone_args args{};
		// We want a mostly independent thread that looks almost like a standalone process, but we want
		// a single virtual memory space.
		// args.flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_CLEAR_SIGHAND | 0 | CLONE_VM | CLONE_PIDFD | CLONE_SETTLS;
		args.flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_CLEAR_SIGHAND | CLONE_PTRACE | CLONE_VM | CLONE_PIDFD | CLONE_SETTLS;
		// NOT CLONE_FILES, CLONE_FS, CLONE_PARENT, CLONE_THREAD, CLONE_VFORK, 
		// Not sure: CLONE_SETTLS
		args.pidfd = 0;
		args.child_tid = (uint64_t) &client.exec_info->tid_in_child;
		args.parent_tid = (uint64_t) &client.exec_info->tid_in_parent;
		// https://sourceware.org/pipermail/gdb/2005-May/022639.html
		args.exit_signal = SIGCHLD;
		args.pidfd = (uint64_t) &client.exec_info->pidfd;

		client.exec_info->stack_size = 256 * 1024 * 1024; // 256 MiB stack
		client.exec_info->stack_area_low = static_cast<std::byte*>(mmap(nullptr, client.exec_info->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
		if (client.exec_info->stack_area_low == MAP_FAILED) {
			std::cerr << "Error allocating stack: " << strerror(errno) << std::endl;
			return;
		}
		client.exec_info->stack_area_high = client.exec_info->stack_area_low + client.exec_info->stack_size;

		args.stack = (uint64_t)client.exec_info->stack_area_low;
		args.stack_size = client.exec_info->stack_size;

		client.exec_info->child_tls_size = 1024 * 1024; // 1 MiB TLS
		std::uint64_t child_tls_pointer = 0;
		if (create_private_tls_copy(client.exec_info->child_tls, client.exec_info->child_tls_size, child_tls_pointer) != 0) {
			munmap(client.exec_info->stack_area_low, client.exec_info->stack_size);
			return;
		}
		args.tls = child_tls_pointer;


		args.set_tid = 0;
		args.set_tid_size = 0;
		args.cgroup = 0;

		std::cerr << "Child stack: " << (void*)args.stack << " - " << (void*)(args.stack + args.stack_size) << std::endl;
		std::cerr << "Child TLS: " << (void*)args.tls << " - " << (void*)(args.tls + client.exec_info->child_tls_size) << std::endl;
		
		int rc = spawn_client_impl(args, &client);
		if (rc == -1) {
			std::cerr << "Error cloning process: " << strerror(errno) << std::endl;
			return;
		}

		// In parent
		client.exec_info->tid_in_parent = rc;
		client.exec_info->parent_errno_after_clone = 2002;
		errno = client.exec_info->parent_errno_after_clone;
		close(client.stderr_fd);
		close(client.stdout_fd);
		close(client.stdin_fd);
		client.status = client_info::status::executing;
		std::cout << "Spawned client process with TID " << rc << std::endl;
		request_waitid(client);

		// TODO spawn client process with client.conn_fd, client.stdin_fd, client.stdout_fd, client.stderr_fd, client.cwd, client.env, and client.args
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
		std::cout << "TLS self-test: parent_tp=" << (void*)client.exec_info->parent_thread_pointer
			<< " child_tp=" << (void*)client.exec_info->child_thread_pointer
			<< " parent_errno_before=" << client.exec_info->parent_errno_before_clone
			<< " parent_errno_after=" << client.exec_info->parent_errno_after_clone
			<< " child_errno_start=" << client.exec_info->child_errno_at_start
			<< " child_errno_after_set=" << client.exec_info->child_errno_after_set
			<< std::endl;

		if (client.exec_info->child_thread_pointer == client.exec_info->parent_thread_pointer) {
			std::cerr << "Warning: child TP equals parent TP; TLS likely still shared" << std::endl;
		}
		if (client.exec_info->child_errno_after_set != 3003) {
			std::cerr << "Warning: child errno write did not stick in child TLS" << std::endl;
		}
		if (client.exec_info->parent_errno_after_clone == client.exec_info->child_errno_after_set) {
			std::cerr << "Warning: parent/child errno markers unexpectedly match" << std::endl;
		}

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

		std::cout << "Notified client of process exit" << std::endl;

		return 0;
	}

	int sockfd;
	io_uring ring;
	intrusive_list<ring_request_info> pending_requests;
	std::vector<std::unique_ptr<client_info>> clients;
};

} // namespace ulab

int main(int argc, char *argv[]) {

	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <socket_path>" << std::endl;
		return 1;
	}

	std::cout << "Server PID " << getpid() << std::endl;

	char const* const socket_path = argv[1];

	struct stat statbuf;
	int rc = fstatat(AT_FDCWD, socket_path, &statbuf, 0);
	if (rc == 0) {
		std::cerr << "Error: file already exists at path '" << socket_path << std::endl;
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
	char const* const last_written = strncpy(&addr.sun_path[0], socket_path, sizeof(addr.sun_path));
	if (last_written == nullptr) {
		perror("Error copying socket path");
		return 1;
	} else if (last_written >= &addr.sun_path[sizeof(addr.sun_path) - 1] || addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
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

	try {
		s.run();
	} catch (...) {
		unlink(socket_path);
		throw;
	}

	unlink(socket_path);

	return 0;
}