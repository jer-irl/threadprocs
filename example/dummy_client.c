#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
		return 1;
	}
	printf("Hello from dummy_client!\n");
	printf("Connecting to server at socket path: %s\n", argv[1]);

	int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sock == -1) {
		perror("socket");
		return 1;
	}
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = {0},
	};
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);
	if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
		fprintf(stderr, "Error: socket path is too long\n");
		return 1;
	}
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("connect");
		return 1;
	}
	printf("Connected to server!\n");
	const char* message = "Hello from dummy_client!";
	if (send(sock, message, strlen(message), 0) == -1) {
		perror("send");
		return 1;
	}
	printf("Message sent, waiting for response...\n");
	char buf[1024];
	ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
	if (n == -1) {
		perror("recv");
		return 1;
	}
	buf[n] = '\0';
	printf("Received from server: %s\n", buf);

	n = recv(sock, buf, sizeof(buf), 0);
	if (n == -1) {
		perror("recv");
		return 1;
	} else if (n == 0) {
		fprintf(stderr, "Server closed connection before sending magic data pointer\n");
		return 1;
	} else if (n != sizeof(char*)) {
		fprintf(stderr, "Received unexpected number of bytes for magic data pointer: %zd\n", n);
		return 1;
	}
	char* magic_data_ptr = (char*) *((char**) buf);
	printf("Received magic data pointer from server: %p\n", magic_data_ptr);
	printf("Reading magic data from server memory: %s\n", magic_data_ptr);

	if (send(sock, "Thanks for the magic data!", 27, 0) == -1) {
		perror("send");
		return 1;
	}
	printf("Sent thank you message to server\n");
	close(sock);

	return 0;
}
