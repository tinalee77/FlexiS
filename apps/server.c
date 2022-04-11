/* This code is adapted from the following source: https://github.com/silviov/TCP-LEDBAT/blob/master/util/server.c */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIRSIZE     8192

typedef struct _options {
	uint16_t port;
	uint8_t conn;
} options_data;

int sfd, cfd; 

void end(char *msg) {
	perror(msg);
	if (close(sfd))
		perror("close sfd");
	if (close(cfd))
		perror("close cfd");
	exit(EXIT_FAILURE);
}

void parse_opt(int argc, char *argv[], options_data * opts)
{
	int opt;

	opts->port = 60000; // server listening port
	opts->conn = 10;

	while ((opt = getopt(argc, argv, "hp:c:")) != -1) {
		switch (opt) {
		case 'p':
			opts->port = atoi(optarg);
			break;
		case 'c':
			opts->conn = atoi(optarg);
			break;
		default:
		case 'h':
			printf("Usage: %s [-p listen_port] [-c max_num_clients_can_accept] \n", argv[0]);
			exit(EXIT_SUCCESS);
		}
	}
}

void my_rec(int rec_socket)
{
	char buffer[DIRSIZE];
	int b;

	for (;;) {
		b = recv(rec_socket, buffer, DIRSIZE, 0);
		if (b == -1) {
			end("recv");
		} else if (b > 0) {
			printf("received a segment\n");
		} else {
			printf("received all\n");
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	options_data options;
	struct sockaddr_in sockaddr;
	int proc = 0, optval;
	pid_t pid;

	parse_opt(argc, argv, &options);
	
	if ((sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		end("socket");
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_addr.s_addr = INADDR_ANY;
	//sockaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	sockaddr.sin_port = htons(options.port);

	if (bind(sfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1) {
		end("bind");
	}

	printf("listening for connections\n");
	
	if (listen(sfd, 10) == -1) {
		end("listen");
	}

	while (proc < options.conn) {
		proc++;
		
		if ((cfd = accept(sfd, NULL, NULL)) == -1) {
			end("accept");
		}

		printf("connected to one client\n");

		switch (pid = fork()) {
		case 0:	//child process
			my_rec(cfd);
			exit(EXIT_SUCCESS);
			break;
		case -1:	//error
			end("fork");
			break;
		default:	//parent process
			break;
		}

	}

	wait(NULL); // waiting for childern to finish
	if (close(sfd))
		perror("close sfd");
	if (close(cfd))
		perror("close cfd");
	return 0;
}
