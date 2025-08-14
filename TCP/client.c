#include "consts.h"
#include "io.h"
#include "transport.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: client <hostname> <port> \n");
        exit(1);
    }

    char* addr = strcmp(argv[1], "localhost") == 0 ? "127.0.0.1" : argv[1];
    int port = atoi(argv[2]);  

    // Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // use IPv4 and UDP

    // Construct server address (to connect to the server)
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;              // use IPv4
    server_addr.sin_addr.s_addr = inet_addr(addr); // inet_addr() converts human-readable address to 32-bit binary 's_addr'
    server_addr.sin_port = htons(port);            // Little -> Big Endian (network order)

    init_io();
    listen_loop(sockfd, &server_addr, CLIENT_START, input_io, output_io);

    return 0;
}