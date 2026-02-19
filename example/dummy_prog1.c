#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <server/client> <socket_path>\n", argv[0]);
		return 1;
	}
	printf("Hello from dummy_prog1!\n");

	bool const is_server = strncmp(argv[1], "server", 6) == 0;
	if (is_server) {
		printf("Running as server, socket path: %s\n", argv[2]);
	} else {
		printf("Running as client, socket path: %s\n", argv[2]);
	}

	if (is_server) {
		int server_sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (server_sock == -1) {
			perror("socket");
			return 1;
		}
		struct sockaddr_un addr = {
			.sun_family = AF_UNIX,
			.sun_path = {0},
		};
		strncpy(addr.sun_path, argv[2], sizeof(addr.sun_path) - 1);
		if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
			fprintf(stderr, "Error: socket path is too long\n");
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
		printf("Server listening on socket...\n");
		int client_sock = accept(server_sock, NULL, NULL);
		if (client_sock == -1) {
			perror("accept");
			return 1;
		}
		printf("Client connected!\n");
		char buf[1024];
		ssize_t n = recv(client_sock, buf, sizeof(buf) - 1, 0);
		if (n == -1) {
			perror("recv");
			return 1;
		}
		buf[n] = '\0';
		printf("Received from client: %s\n", buf);
		printf("Sending back to client...\n");
		const char* response = "Hello from dummy_prog1 server!";
		if (send(client_sock, response, strlen(response), 0) == -1) {
			perror("send");
			return 1;
		}
		printf("Response sent\n");


		char* magic_data_spot = (char*) malloc(4096);
		if (magic_data_spot == NULL) {
			perror("malloc");
			return 1;
		}
		strncpy(magic_data_spot, "This is some magic data that the client should find in the server's memory!", 4096);
		printf("Magic data allocated at %p\n", magic_data_spot);
		printf("Magic data content: %s\n", magic_data_spot);

		if (send(client_sock, &magic_data_spot, sizeof(magic_data_spot), 0) == -1) {
			perror("send");
			return 1;
		}
		printf("Magic data pointer sent to client\n");

		n = recv(client_sock, buf, sizeof(buf) - 1, 0);
		if (n == -1) {
			perror("recv");
			return 1;
		}
		buf[n] = '\0';
		printf("Received from client: %s\n", buf);
		close(client_sock);
		close(server_sock);

	} else {
		int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (sock == -1) {
			perror("socket");
			return 1;
		}
		struct sockaddr_un addr = {
			.sun_family = AF_UNIX,
			.sun_path = {0},
		};
		strncpy(addr.sun_path, argv[2], sizeof(addr.sun_path) - 1);
		if (addr.sun_path[sizeof(addr.sun_path) - 1] != '\0') {
			fprintf(stderr, "Error: socket path is too long\n");
			return 1;
		}
		printf("Connecting to server...\n");
		if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
			perror("connect");
			return 1;
		}
		printf("Connected to server!\n");
		const char* message = "Hello from dummy_prog1 client!";
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
	}


	return 0;
}