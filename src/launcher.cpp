#include "protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <unistd.h>

#include <cstring>
#include <csignal>
#include <filesystem>
#include <iostream>

namespace ulab {

void sendfd(int sockfd, int fd_to_send, ClientRequest::Kind request_type) {
	struct msghdr msg{};
	struct iovec iov;
	ClientRequest request{request_type, sizeof(ClientRequest)};
	iov.iov_base = &request;
	iov.iov_len = sizeof(request);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	union {
		char control_buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} control_un;

	msg.msg_control = control_un.control_buf;
	msg.msg_controllen = sizeof(control_un.control_buf);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*(int *) CMSG_DATA(cmsg) = fd_to_send;

	int rc = sendmsg(sockfd, &msg, 0);
	if (rc == -1) {
		std::cerr << "Error sending file descriptor: " << strerror(errno) << std::endl;
		throw std::runtime_error("Failed to send file descriptor");
	}
}

int sockfd = -1;

} // namespace ulab

int main(int argc, char *argv[]) {

	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <socket_path> <executable_path> [args...]" << std::endl;
		return 1;
	}

	std::cout << "Launcher PID " << getpid() << std::endl;

	char const* const socket_path = argv[1];

	ulab::sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (ulab::sockfd == -1) {
		std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
		return 1;
	}

	sockaddr_un addr{
		.sun_family = AF_UNIX,
		.sun_path = {0},
	};
	std::strncpy(&addr.sun_path[0], socket_path, sizeof(addr.sun_path) - 1);
	if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
		std::cerr << "Error: socket path is too long" << std::endl;
		return 1;
	}

	int rc = connect(ulab::sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0) {
		std::cerr << "Error connecting to socket: " << strerror(errno) << std::endl;
		return 1;
	}

	ulab::sendfd(ulab::sockfd, STDIN_FILENO, ulab::ClientRequest::Kind::stdin_fd);
	ulab::sendfd(ulab::sockfd, STDOUT_FILENO, ulab::ClientRequest::Kind::stdout_fd);
	ulab::sendfd(ulab::sockfd, STDERR_FILENO, ulab::ClientRequest::Kind::stderr_fd);

	auto cwd = std::filesystem::current_path().string();
	
	char buf[4096];
	// TODO be rigorous about sizes, buffers
	ulab::ClientRequest& request = *reinterpret_cast<ulab::ClientRequest*>(buf);
	request.type = ulab::ClientRequest::Kind::cwd;
	request.total_len = sizeof(request) + cwd.size() + 1;
	std::strncpy(request.payload[0].cwd, cwd.c_str(), sizeof(buf) - sizeof(request) - 1);
	rc = send(ulab::sockfd, buf, request.total_len, 0);
	if (rc == -1) {
		std::cerr << "Error sending cwd: " << strerror(errno) << std::endl;
		return 1;
	}

	request.type = ulab::ClientRequest::Kind::args;
	size_t offset = 0;
	for (int i = 2; i < argc; i++) {
		size_t arg_len = std::strlen(argv[i]) + 1;
		if (offset + arg_len > sizeof(buf) - sizeof(request) - sizeof(request.payload[0].args.argc)) {
			std::cerr << "Error: total length of arguments exceeds buffer size" << std::endl;
			return 1;
		}
		std::strncpy(&buf[offsetof(ulab::ClientRequest, payload[0].args.argz[0]) + offset], argv[i], arg_len);
		offset += arg_len;
	}
	request.payload[0].args.argc = argc - 2;
	request.total_len = sizeof(request) + offset + sizeof(request.payload[0].args.argc);
	rc = send(ulab::sockfd, buf, request.total_len, 0);

	if (rc == -1) {
		std::cerr << "Error sending arguments: " << strerror(errno) << std::endl;
		return 1;
	}

	request.type = ulab::ClientRequest::Kind::env;
	offset = 0;
	int num_vars = 0;
	for (char** env = environ; *env != nullptr; env++) {
		size_t var_len = std::strlen(*env) + 1;
		if (offset + var_len > sizeof(buf) - sizeof(request) - sizeof(request.payload[0].env.num_vars)) {
			std::cerr << "Error: total length of environment variables exceeds buffer size" << std::endl;
			return 1;
		}
		std::strncpy(&buf[offsetof(ulab::ClientRequest, payload[0].env.vars[0]) + offset], *env, var_len);
		offset += var_len;
		num_vars++;
	}
	request.payload[0].env.num_vars = num_vars;
	request.total_len = sizeof(request) + offset + sizeof(request.payload[0].env.num_vars);

	rc = send(ulab::sockfd, buf, request.total_len, 0);
	if (rc == -1) {
		std::cerr << "Error sending environment variables: " << strerror(errno) << std::endl;
		return 1;
	}

	// Install signal handlers, TODO race condition.
	const auto sighandler = +[](int signum) {
		char buf[4096];
		// TODO rigorous about sizes, buffers-
		ulab::ClientRequest &request = *reinterpret_cast<ulab::ClientRequest*>(buf);
		size_t const len = sizeof(request) + sizeof(request.payload[0].signal);
		request.type = ulab::ClientRequest::Kind::signal;
		request.total_len = len;
		request.payload[0].signal.signo = signum;
		int rc = send(ulab::sockfd, &request, len, 0);
		if (rc == -1) {
			std::cerr << "Error sending signal notification: " << strerror(errno) << std::endl;
		} else {
			std::cout << "Sent signal notification for signal " << signum << std::endl;
		}
	};

	for (int signum = 1; signum < NSIG; signum++) {
		auto ptr = std::signal(signum, sighandler);
		if (ptr == SIG_ERR) {
			std::cerr << "Warning: failed to set signal handler for signal " << signum << ": " << strerror(errno) << std::endl;
		}
	}

	msghdr msg{};
	msg.msg_iovlen = 1;
	iovec iov;
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	msg.msg_iov = &iov;	
	msg.msg_control = nullptr;
	msg.msg_controllen = 0;
	rc = recvmsg(ulab::sockfd, &msg, 0);
	if (rc < 0) {
		std::cerr << "Error receiving notification: " << strerror(errno) << std::endl;
		return 1;
	}
	if (rc == 0) {
		std::cerr << "Server closed connection before sending notification" << std::endl;
		return 1;
	}

	ulab::ServerNotification& notification = *reinterpret_cast<ulab::ServerNotification*>(buf);
	std::cout << "Received notification from server" << std::endl;
	switch (notification.type) {
		case ulab::ServerNotification::Kind::child_exit:
			std::cout << "Child process with TID " << notification.child_exit.tid << " exited with status " << notification.child_exit.exit_status << std::endl;
			break;
	}

	return 0;
}
