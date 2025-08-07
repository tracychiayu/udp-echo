#include <arpa/inet.h>  // IP address
#include <fcntl.h>      // manage file descriptor's flags
#include <stdio.h>      // input/output functions
#include <stdlib.h>     // exit(), atoi()
#include <string.h>
#include <sys/socket.h> // socket interface
#include <unistd.h>     // close, usleep
#include <errno.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: client <hostname> <port> \n");
        exit(1);
    }

    // Only supports localhost as a hostname
    const char* addr =
        strcmp(argv[1], "localhost") == 0 ? "127.0.0.1" : argv[1];  // if argv[1] == "localhost", use "127.0.0.1"; else, use argv[1]
    int port = atoi(argv[2]);  // string to int

    // Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // use IPv4, use UDP
    if (sockfd < 0){
        perror("[ERROR] Fail to create socket.\n");
        exit(1);
    }

    // Set stdin and socket nonblocking
    // 1. Set socket to be nonblocking
    int socket_flags = fcntl(sockfd, F_GETFL);
    socket_flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, socket_flags);

    // 2. Set stdin to be nonblocking
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    stdin_flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);

    // Construct server address (to connect to the server)
    struct sockaddr_in serveraddr = {0};           // zeros out every field in the struct
    serveraddr.sin_family = AF_INET;               // set address family to IPv4
    serveraddr.sin_addr.s_addr = inet_addr(addr);  // inet_addr() converts human-readable address to 32-bit binary `s_addr`
    serveraddr.sin_port = htons(port);             // Little -> Big Endian (network order)

    socklen_t serversize = sizeof(serveraddr);     // struct size
    int BUFF_SIZE = 1024;
    char recv_buffer[BUFF_SIZE];  // buffer to store messages from the server
    char send_buffer[BUFF_SIZE];  // stores message to be sent to the server

    // // DEBUG
    // const char* test_msg = "hello from client\n";
    // ssize_t sent = sendto(sockfd, test_msg, strlen(test_msg), 0,
    //                     (struct sockaddr*)&serveraddr, serversize);
    // fprintf(stderr, "[DEBUG] Sent %zd bytes to server\n", sent);

    // Listen loop
    while(1){
        // A. Receive from socket and write to STDOUT
        struct sockaddr_in recv_addr;
        socklen_t recv_addrlen = sizeof(recv_addr);     
        int bytes_recvd = recvfrom(sockfd, recv_buffer, BUFF_SIZE, 0, (struct sockaddr*)&recv_addr, &recv_addrlen);

        // If data is received from the socket, write to STDOUT
        if (bytes_recvd > 0){
            fprintf(stderr,"[DEBUG] Received %d bytes from server\n", bytes_recvd);
            write(STDOUT_FILENO, recv_buffer, bytes_recvd);

        }

        // No message received from the server yet; continue listening
        else if ( bytes_recvd == -1 && errno != EAGAIN && errno != EWOULDBLOCK){ 
            fprintf(stderr, "[ERROR] recvfrom() failed to receive data from server.\n");
            exit(1);
        }
    
        // B. Read from STDIN and send to socket
        int bytes_read = read(STDIN_FILENO, send_buffer, BUFF_SIZE);
        
        if (bytes_read > 0){
            fprintf(stderr,"[DEBUG] Read %d bytes from STDIN.\n", bytes_read);
            
            // If data is available at STDIN, send to socket
            ssize_t did_send = sendto(sockfd, send_buffer, bytes_read, 0, (struct sockaddr*) &serveraddr, serversize);
            fprintf(stderr,"[DEBUG] %zd bytes are sent to server.\n", did_send);

            if (did_send < 0){
                fprintf(stderr, "[ERROR] sendto() failed to send data to server.\n");
                exit(1);
            }
        }
        else if (bytes_read == -1){
            // No data available from STDIN yet; continue listening
            if (errno == EAGAIN || errno == EWOULDBLOCK){ continue; }
            fprintf(stderr, "[ERROR] read() failed to read data from STDIN.\n");
            exit(1);
        }
        else if(bytes_read == 0){
            continue;
        }

        // Pause for 10ms. Avoid 100% CPU usage and allow the system to process I/O
        usleep(1000000);
    }

    close(sockfd);

    return 0;
}