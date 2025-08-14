#include "consts.h"
#include "transport.h"
#include "io.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: server <port>\n");
        exit(1);
    }

    int port = atoi(argv[1]);

    // Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // use IPv4 and UDP

    // Construct server address (to accept connection)
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;           // use IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;   // accept connections from any IP address
                                                // same as inet_addr("0.0.0.0")
    server_addr.sin_port = htons(port);         // Little -> Big Endian

    // Bind address to socket
    bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr));

    // Create struct to store client's address
    struct sockaddr_in client_addr = {0};       // zeros out every field in the struct
    socklen_t s = sizeof(client_addr);
    char buffer;

    // Wait for client connection
    recvfrom(sockfd, &buffer, sizeof(buffer), MSG_PEEK, (struct sockaddr*) &client_addr, &s);

    init_io();
    listen_loop(sockfd, &client_addr, SERVER_AWAIT, input_io, output_io);

    return 0;
}