/*
** client.c -- a stream socket client demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arpa/inet.h>

// #define PORT "80" // the port client will be connecting to 

#define MAXDATASIZE 1024 // max number of bytes we can get at once 

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
	int sockfd, numbytes;
	int is_header;  
	char buf[MAXDATASIZE];
	char request[1024];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char *hostname, *port, *path_to_file;
	FILE *output;
	char PORT[6] = "80";
	char *arg_ptr;
	char s[INET6_ADDRSTRLEN];


	if (argc != 2) {
	    fprintf(stderr,"usage: client hostname\n");
	    exit(1);
	}

	// parse argv[1] to hostname, port, path_to_file
	arg_ptr = strstr(argv[1], "://");
	if (NULL == arg_ptr) {
		fprintf(stderr, "usage: no protocal specified in argument\n");
		exit(1);
	}

	arg_ptr += 3;
	hostname = arg_ptr;
	port = PORT;
	if (strstr(arg_ptr, ":") != NULL) {
		hostname = strtok(hostname, ":");
		port = strtok(NULL, "/");
		// check port range in [0, 65535]
		char *endptr;
		long num = strtol(port, &endptr, 10);
		if (*endptr != '\0' || num > 65535) {
			// error occurred, string is not a valid integer
			fprintf(stderr, "usage: invalid port\n");
			exit(1);
		}
		path_to_file = strtok(NULL, "");
	} else {
		hostname = strtok(hostname, "/");
		path_to_file = strtok(NULL, "");
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(hostname, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("client: connect");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		return 2;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			s, sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure

	// // set timeout to break connection
	struct timeval tv;
	tv.tv_sec = 10;  // 10 second timeout
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
	// make request
	sprintf(request, "GET /%s HTTP/1.1\r\nUser-Agent: Wget/1.12(linux-gnu)\r\nHost: %s:%s\r\nConnection: Keep-Alive\r\n\r\n", path_to_file, hostname, port);
	if (send(sockfd, request, strlen(request), 0) == -1) {
		close(sockfd);
		perror("send");
		exit(1);
	}

	output = fopen("output", "w");
	if (output == NULL) {
		close(sockfd);
		perror("fopen");
		exit(1);
	}
	// receive HTTP response
	is_header = 1;
	printf("client: received header:\n");
	// buf[MAXDATASIZE - 1] = '\0';
	// long nn = 0;
	memset(buf,'\0', MAXDATASIZE);
	while ((numbytes = recv(sockfd, buf, MAXDATASIZE - 1, 0)) > 0) {
		if (is_header && (strstr(buf, "\r\n\r\n") != NULL)) {
			char *temp_ptr;
			int remain_bytes = numbytes;
			is_header = 0;
			// extract header portion
			temp_ptr = strstr(buf, "\r\n\r\n");
			*temp_ptr = '\0';
			// temp_ptr = strtok(buf, "\r\n\r\n");
			printf("%s\n",buf);
			// check remaining bytes
			remain_bytes -= strlen(buf) + 4;
			if (remain_bytes > 0) {
				// temp_ptr = strtok(NULL, "") + 3;
				temp_ptr += 4;
				fwrite(temp_ptr, sizeof(char), remain_bytes, output);
				// nn += remain_bytes;
			}
			continue;
		}
		if (is_header) {
			printf("%s",buf);
		} else {
			fwrite(buf, sizeof(char), numbytes, output);
			// nn += numbytes;
		}
		// memset(buf,'\0', MAXDATASIZE);
	}
	// printf("%ld", nn);
	if (numbytes == -1) {
		fclose(output);
		close(sockfd);
	    perror("recv");
	    exit(1);
	}

	// buf[numbytes] = '\0';


	fclose(output);
	close(sockfd);

	return 0;
}

