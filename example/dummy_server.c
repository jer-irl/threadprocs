#include "dummy_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int sync_count = 0;
char proc_name[] = "dummy_server";
void do_sync(int);

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
		return 1;
	}
	DUMMY_PRINT("Hello from dummy_server!\n");
	DUMMY_PRINT("Running as server, socket path: %s\n", argv[1]);

	int server_sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (server_sock == -1) {
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
	if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		return 1;
	}
	if (listen(server_sock, 1) == -1) {
		perror("listen");
		return 1;
	}
	DUMMY_PRINT("Server listening on socket...\n");
	int client_sock = accept(server_sock, NULL, NULL);
	if (client_sock == -1) {
		perror("accept");
		return 1;
	}
	do_sync(client_sock);
	DUMMY_PRINT("Client connected!\n");
	char buf[1024];
	ssize_t n = recv(client_sock, buf, sizeof(buf) - 1, 0);
	if (n == -1) {
		perror("recv");
		return 1;
	}
	buf[n] = '\0';
	DUMMY_PRINT("Received from client: %s\n", buf);
	DUMMY_PRINT("Sending back to client...\n");
	const char* response = "Hello from dummy_server!";
	if (send(client_sock, response, strlen(response), 0) == -1) {
		perror("send");
		return 1;
	}
	DUMMY_PRINT("Response sent\n");
	do_sync(client_sock);

	char* magic_data_spot = (char*) malloc(4096);
	if (magic_data_spot == NULL) {
		perror("malloc");
		return 1;
	}
	strncpy(magic_data_spot, "This is some magic data that the client should find in the server's memory!", 4096);
	DUMMY_PRINT("Magic data allocated at %p\n", magic_data_spot);
	DUMMY_PRINT("Magic data content: %s\n", magic_data_spot);

	if (send(client_sock, &magic_data_spot, sizeof(magic_data_spot), 0) == -1) {
		perror("send");
		return 1;
	}
	DUMMY_PRINT("Magic data pointer sent to client\n");
	do_sync(client_sock);

	n = recv(client_sock, buf, sizeof(buf) - 1, 0);
	if (n == -1) {
		perror("recv");
		return 1;
	}
	buf[n] = '\0';
	DUMMY_PRINT("Received from client: %s\n", buf);
	do_sync(client_sock);
	close(client_sock);
	close(server_sock);

	return 0;
}
