#pragma once

#include <stdio.h>
#include <sys/socket.h>

extern int sync_count;

inline void do_sync(int sockfd)
{
	printf("Syncing %d: ", sync_count);
	char b = 'a';
	send(sockfd, &b, 1, 0);
	recv(sockfd, &b, 1, 0);
	printf("DONE\n");
	sync_count += 1;
}
