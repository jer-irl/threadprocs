#include "protocol.hpp"
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

#include <iostream>
#include <unistd.h>

namespace ulab {

void sendfd(int sockfd, int fd_to_send, client_request_type request_type) {
	struct msghdr msg{};
	struct iovec iov;
	client_request request{request_type, sizeof(client_request)};
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

} // namespace ulab


int main(int argc, char *argv[]) {

	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <socket_path> <executable_path> [args...]" << std::endl;
		return 1;
	}

	char const* const socket_path = argv[1];

	int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sockfd == -1) {
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

	int rc = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0) {
		std::cerr << "Error connecting to socket: " << strerror(errno) << std::endl;
		return 1;
	}

	ulab::sendfd(sockfd, STDIN_FILENO, ulab::client_request_type::stdin_fd);
	ulab::sendfd(sockfd, STDOUT_FILENO, ulab::client_request_type::stdout_fd);
	ulab::sendfd(sockfd, STDERR_FILENO, ulab::client_request_type::stderr_fd);

	return 0;
}
