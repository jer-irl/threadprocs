#pragma once

#include <stdio.h>
#include <sys/socket.h>

extern int sync_count;
extern char proc_name[];

#define DUMMY_PRINT(s, args...) (void) printf("%s: " s, proc_name, ##args);

inline void do_sync(int sockfd)
{
	DUMMY_PRINT("Syncing %d: ", sync_count);
	char b = 'a';
	send(sockfd, &b, 1, 0);
	recv(sockfd, &b, 1, 0);
	DUMMY_PRINT("DONE\n");
	sync_count += 1;
}
