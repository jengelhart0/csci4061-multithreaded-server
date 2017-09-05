/* csci4061 S2016 Optional Assignment
 * section: 7
 * date: 12/27/16
 * name: Joey Engelhart
 * UMN Internet ID, Student ID: engel429
 */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include "util.h"

#define MAXBACKLOG 50
#define MAXFILENAME 1024
#define MAXHEADERSIZE 128 

const char *CONTENT_TYPE = "Content-Type: ";
const char *CONTENT_LENGTH = "Content-Length: ";
const char *CONNECTION = "Connection: close";
const char *BLANK_LINE = "\n";

int listening_sock;

void init(int port) {
	struct sockaddr_in server;
	
	if((listening_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("Attempt to establish socket produced error. Exiting:");
		exit(-1);	
 	}	
	server.sin_family = AF_INET;
	server.sin_port = htons((short) port);
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	
	int enable = 1;
	if(setsockopt(listening_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &enable, sizeof(int)) == -1) {
		perror("Attempt to set socket options produced error: Exiting:");
		exit(-1);
	}
	if(bind(listening_sock, (struct sockaddr*) &server, sizeof(server))) {
		perror("Attempt to bind port to socket failed: Exiting:");
		exit(-1);
	}
	if(listen(listening_sock, MAXBACKLOG)) {
		perror("Attempt to listen on socket failed: Exiting:");
		exit(-1);
	}
}

int accept_connection(void) {
	int private_sock;	
	while(((private_sock = accept(listening_sock, NULL, NULL)) == -1) &&
	      (errno == EINTR));

	if(private_sock == -1) {
		perror("Attempt to accept connection failed: exiting");
		return -1;
	}

	return private_sock;
}

int get_request(int fd, char *filename) {
	int getsize = 5; // size of "GET " plus null terminator
	char request_type[getsize];

	FILE *sock_stream;
	// duplicate fd so when we close the stream, we don't destroy connection
	if(!(sock_stream = fdopen(dup(fd), "r+"))) {
		perror("Attempt to open sock as FILE stream failed in get_request");
		return -1;
	}
	if(!fgets(request_type, getsize, sock_stream)) {
		fprintf(stderr, "Failed to get request type in get_request\n");
		fclose(sock_stream);
		return -1;
	}
	if(strcmp(request_type, "GET ")) {
		fprintf(stderr, "In get_request(): Request type not GET\n");	
		fclose(sock_stream);
		return -1;
	}
	if(!fgets(filename, MAX_REQUEST_LENGTH - getsize, sock_stream)) {
		fprintf(stderr, "In get_request(): Failed to read request\n");
		fclose(sock_stream);
		return -1;
	}

	// null terminate at the character immediately following the file request
	char *runner = filename;
	while((runner != NULL) && (*runner != ' ')) {
		runner++;
	}
	if(runner == NULL) {
		fprintf(stderr, "In get_request(): Invalid request: No protocol listed after filename or invalid filename\n");
		fclose(sock_stream);
		return -1;
	}
	*runner = '\0';

	// verify no '//' exists in filename
	runner = filename;
	char cur, next;
	while(*runner++ != '\0') {
		if(((cur = *runner) == '/') && 
		   ((next = *(runner + 1)) == '/')) {
			return_error(fd, "In get_request: Illegal char sequence in request '//'\n");
			fclose(sock_stream);
			return -1;
		} else if(cur == '.' && next == '.') {
			return_error(fd, "In get_request: Illegal char sequence in request '..'\n");
			fclose(sock_stream);
			return -1;
		}
	}
	fclose(sock_stream);
	return 0;
}

int send_response(int fd, char *status, char *type, char *buf, int numbytes) {
	char header[MAXHEADERSIZE];
	int close_error;
			    
	if((snprintf(header, MAXHEADERSIZE, "%s\n%s%s\n%s%d\n%s\n%s", status, CONTENT_TYPE, type, CONTENT_LENGTH, numbytes, 
	            CONNECTION, BLANK_LINE)) == -1) {
		perror("Failed to create header");	
		while(((close_error = close(fd)) == -1) && (errno == EINTR));
		if(close_error) {
			perror("Failed to close socket");
		}	
		return -1;
	}
	if((write(fd, header, strlen(header)) < strlen(header)) || (write(fd, buf, numbytes) < numbytes)) {
		perror("Failed to send response to client");
		while(((close_error = close(fd)) == -1) && (errno == EINTR));
		if(close_error) {
			perror("Failed to close socket");
		}
		return -1;
	}
	while(((close_error = close(fd)) == -1) && (errno == EINTR));
	if(close_error) {
		perror("Failed to close socket");
	}
	return 0;
}

int return_result(int fd, char *content_type, char *buf, int numbytes) {
	if(send_response(fd, "HTTP/1.1 200 OK", content_type, buf, numbytes) == -1) {
		fprintf(stderr, "In return_result(): Failed to send_response\n");
		return -1;
	}
	return 0;
}

int return_error(int fd, char *buf) {
	if(send_response(fd, "HTTP/1.1 404 Not Found", "text/html", buf, 0) == -1) {
		fprintf(stderr, "In return_error(): Failed to send_response\n");
		return -1;
	}
	return 0;
}
