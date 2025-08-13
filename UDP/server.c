#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stdout, "Usage: server <port>\n");
        exit(1);
    }

    int port = atoi(argv[1]);  // string to int

    // TODO: Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // use IPv4, use UDP
    if (sockfd < 0){ 
        perror("socket");
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


    // Construct server address (to accept connection)
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;  // accept connections from any IP address
    servaddr.sin_port = htons(port);        // Little -> Big Endian 

    // Bind address to socket
    int did_bind = bind(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr));
    if (did_bind < 0){ 
        fprintf(stderr, "[ERROR] Fail to bind address to socket.\n");
        exit(1);
    }
    
    // Create address struct to store client address
    struct sockaddr_in clientaddr = {0};        // zeros out every field in the struct  
    socklen_t clientsize = sizeof(clientaddr);

    int BUFF_SIZE = 1024;
    char recv_buffer[BUFF_SIZE];  // buffer to store messages from the client
    char send_buffer[BUFF_SIZE];  // stores message to be sent to the client
    int client_connected = 0;

    // Listen loop
    while (1){  
        // A. Received from socket and write to STDOUT
        int bytes_recvd = recvfrom(sockfd, recv_buffer, BUFF_SIZE, 0, (struct sockaddr*) &clientaddr, &clientsize); 

        // If data is received from the socket, client is connected and write data to STDOUT
        if (bytes_recvd > 0){
            // fprintf(stderr,"\n[DEBUG] Received %d bytes from client\n", bytes_recvd);
            write(STDOUT_FILENO, recv_buffer, bytes_recvd);
            client_connected = 1;    // indicates that we have the address of client
        }

        // If no data comes in and client is not connected
        else if (bytes_recvd == -1 && errno != EAGAIN && errno != EWOULDBLOCK){
            fprintf(stderr, "[ERROR] recvfrom() failed to receive data from client.\n");
            exit(1);
        }

        // B. Read from STDIN and send to socket if client address is known
        if (client_connected){  

            int bytes_read = read(STDIN_FILENO, send_buffer, BUFF_SIZE); 

            if (bytes_read > 0){ 
                // fprintf(stderr,"[DEBUG] Read %d bytes from STDIN.\n", bytes_read); 

                ssize_t did_send = sendto(sockfd, send_buffer, bytes_read, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
                // fprintf(stderr,"[DEBUG] %zd bytes are sent to client.\n", did_send);

                if (did_send < 0){
                    fprintf(stderr, "[ERROR] sendto() failed to send data to client.\n");
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
        }
    }

    close(sockfd);

    return 0;
}