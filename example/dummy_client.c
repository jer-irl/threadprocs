#include "dummy_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int sync_count = 0;
char proc_name[] = "dummy_client";
void do_sync(int);

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
		return 1;
	}
	DUMMY_PRINT("Hello from dummy_client!\n");
	DUMMY_PRINT("Connecting to server at socket path: %s\n", argv[1]);

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
		DUMMY_PRINT("Error: socket path is too long\n");
		return 1;
	}
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("connect");
		return 1;
	}
	do_sync(sock);
	DUMMY_PRINT("Connected to server!\n");
	const char* message = "Hello from dummy_client!";
	if (send(sock, message, strlen(message), 0) == -1) {
		perror("send");
		return 1;
	}
	DUMMY_PRINT("Message sent, waiting for response...\n");
	char buf[1024];
	ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
	if (n == -1) {
		perror("recv");
		return 1;
	}
	buf[n] = '\0';
	DUMMY_PRINT("Received from server: %s\n", buf);
	do_sync(sock);
	n = recv(sock, buf, sizeof(buf), 0);
	if (n == -1) {
		perror("recv");
		return 1;
	} else if (n == 0) {
		DUMMY_PRINT("Server closed connection before sending magic data pointer\n");
		return 1;
	} else if (n != sizeof(char*)) {
		DUMMY_PRINT("Received unexpected number of bytes for magic data pointer: %zd\n", n);
		return 1;
	}
	char* magic_data_ptr = (char*) *((char**) buf);
	DUMMY_PRINT("Received magic data pointer from server: %p\n", magic_data_ptr);
	DUMMY_PRINT("Reading magic data from server memory: %s\n", magic_data_ptr);
	do_sync(sock);

	if (send(sock, "Thanks for the magic data!", 27, 0) == -1) {
		perror("send");
		return 1;
	}
	DUMMY_PRINT("Sent thank you message to server\n");
	do_sync(sock);
	close(sock);

	return 0;
}
