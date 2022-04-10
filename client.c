#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#define DIRSIZE 65536
#define TCP_CA_NAME_MAX 16

typedef struct _options {
	char *srv_ip;
	char *cc;
	uint16_t srv_port;
	uint32_t count;
	uint16_t duration;
} options_data;

int sd = 0;

static void handler(int sig, siginfo_t *si, void *unused)
{
	if (close(sd))
		perror("close");
	exit(EXIT_SUCCESS);
}

void end(char *msg) {
	perror(msg);
	if (close(sd))
		perror("close");
	exit(EXIT_FAILURE);
}

void parse_opt(int argc, char *argv[], options_data * opts)
{
	int opt;

	opts->srv_ip = NULL;
	opts->cc = "xyz";
	opts->srv_port = 60000;
	opts->count = 0;
	opts->duration = 0;

	while ((opt = getopt(argc, argv, "ha:p:m:c:t:")) != -1) {
		switch (opt) {
		case 'a':
		        opts->srv_ip = optarg;
		        break;
		case 'p':
			opts->srv_port = atoi(optarg);
			break;
		case 'm':
			opts->cc = optarg;
			break;
		case 'c':
			opts->count = atoi(optarg);
			break;
		case 't':
			opts->duration = atoi(optarg);
			break;
		default:
		case 'h':
			printf("Usage: %s <-a server_ipv4_address> [-p server_port] [-m cc mechanism] [-t duration] [-c count]\n", argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	if (!opts->srv_ip) {
		printf("Please specify server ipv4 address\n");
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	char dir[DIRSIZE];
	char optval[TCP_CA_NAME_MAX];
	int n = DIRSIZE, count = 0, optlen;
	struct sockaddr_in pin;
	struct sigaction sa;
	options_data opts;

	parse_opt(argc, argv, &opts);

	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handler;
	
	if (sigaction(SIGALRM, &sa, NULL) == -1) {
		end("sigalarm");
	}
	
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		end("sigint");
	}
	
	printf("sigaction successful\n");

	/* grab an Internet domain socket */
	if ((sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		end("socket");
	}
	
	printf("sock creation successful\n");

	/*set the congestion control algorithm to xyz */
	if (setsockopt(sd, SOL_TCP, TCP_CONGESTION, opts.cc, strlen(opts.cc)) == -1) {
		end("setsockopt");
	}

	printf("setsockopt successful\n");

	/* fill in the socket structure with host information */
	memset(&pin, 0, sizeof(pin));
	pin.sin_family = AF_INET;
	pin.sin_addr.s_addr = inet_addr(opts.srv_ip);
	pin.sin_port = htons(opts.srv_port);
	
	/* connect to the host */
	if (connect(sd, (struct sockaddr *)&pin, sizeof(pin)) == -1) {
		end("connect");
	}

	printf("connect successful\n");
	
	optlen = TCP_CA_NAME_MAX;
	if (getsockopt(sd, IPPROTO_TCP, TCP_CONGESTION, optval, &optlen) == -1) {
		end("getsockopt");
	}
	
	printf("current CA: optlen: %d, optval: %s\n", optlen, optval);

	if (opts.duration > 0) {
		alarm(opts.duration);
	}

	memset(dir, 1, DIRSIZE);
	while (1) {
		if (send(sd, dir, n, 0) == -1) {
			end("send");
		}

		if (opts.count > 0) {
			count += n;
			if (count > opts.count)
				break;
		}
	}
	
	close(sd);
	exit(EXIT_SUCCESS);
}
