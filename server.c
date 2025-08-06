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

    int port = atoi(argv[1]);  // getting port number from command line when doing ./server <port>

    // printf("[DEBUG] Enter main()\n");

    // TODO: Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // use IPv4, use UDP
    if (sockfd < 1){ 
        perror("socket");
        exit(1); 
    }
    // printf("[DEBUG] Socket created\n");
    // TODO: Set stdin and socket nonblocking
    // 1. Set socket to be nonblocking
    int socket_flags = fcntl(sockfd, F_GETFL);
    socket_flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, socket_flags);

    // 2. Set stdin to be nonblocking
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    stdin_flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);


    // TODO: Construct server address (to accept connection)
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;  // accept connections from any IP address

    servaddr.sin_port = htons(port);       // Little -> Big Endian 

    // TODO: Bind address to socket
    int did_bind = bind(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr));
    if (did_bind < 0){ 
        perror("bind");
        exit(1);
    }
    
    // TODO: Create sockaddr_in and socklen_t buffers to store client address

    char recv_buffer[1024];
    int client_connected = 0;

    struct sockaddr_in clientaddr = {0};   // zeros out all field in `clientaddr`
    // struct sockaddr_in clientaddr;   // **
    // clientaddr.sin_family = AF_INET; // **
    // clientaddr.sin_addr.s_addr = inet_addr("127.0.0.1"); // **
    // clientaddr.sin_port = htons(port);  // **
    socklen_t clientsize = sizeof(clientaddr);

    char send_buffer[1024];  // Stores message from stdin & sent to the client; `send_buffer` is a pointer to the first byte of the array

    // Listen loop
    int count = 0;
    int count_stdin = 0;
    int count_recv = 0;
    int received_from_client = 0;
    int input_from_stdin = 0;
    while (1) {  // "Interleavingly executed" recvfrom(...) and read(...). They won't interrupt each other since we're using non-blocking socket & stdin
        count += 1;
        // fprintf(stdout,"[DEBUG] Enter Listen loop, count: %d\n", count);
        

        // TODO: Receive from socket
        // clientsize = sizeof(clientaddr);  // reset clientsize each time
        while (1){  // drain all available UDP packets

            count_recv += 1;
            // fprintf(stdout,"[DEBUG] Enter recvfrom(sockfd) loop, count: %d\n", count_recv); 
            
            errno = 0;
            ssize_t bytes_recvd = recvfrom(sockfd, recv_buffer, 1024, 0, (struct sockaddr*) &clientaddr, &clientsize); // src_addr: clientaddr

            // TODO: If data, client is connected and write to stdout
            if (bytes_recvd > 0){
                fprintf(stderr,"\n[DEBUG] Received %zd bytes from client\n", bytes_recvd);
                write(STDOUT_FILENO, recv_buffer, bytes_recvd);
                client_connected = 1;         // means that we have the address of client
            }

            // TODO: If no data and client not connected
            else if (bytes_recvd < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK){ 
                    // fprintf(stdout, "[DEBUG] No message from client (EAGAIN).\n");
                    break;  // no more data to be received
                }
                perror("recvfrom");
                exit(1);
            }
            else if (bytes_recvd == 0){
                break;
            }
        }

        // TODO: Read from stdin (send message back to the client)
        // TODO: If data, send to socket
        // errno = 0;
        while (client_connected){  // drain all available stdin input and send them to client

            count_stdin += 1;
            // fprintf(stdout,"[DEBUG] Enter read(STDIN)loop, count: %d\n", count_stdin); 
            errno = 0;
            ssize_t bytes_read = read(STDIN_FILENO, send_buffer, 1024);  // reads the input from terminal into `send_buffer`; Reads up to 1024 bytes at a time
            fprintf(stderr,"\n[DEBUG] %zd bytes read from server's STDIN\n.", bytes_read);
            if (bytes_read > 0 && client_connected){  // make sure the client has been connected so we know where to send out message to
                
                ssize_t did_send = sendto(sockfd, send_buffer, bytes_read, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr)); // dest_addr: clientaddr
                if (did_send < 0){
                    perror("sendto");
                    exit(1);
                }
            }
            else if (bytes_read == 0){ break; }
            else if (bytes_read < 0 ){
                if (errno == EAGAIN || errno == EWOULDBLOCK){ 
                    // fprintf(stdout, "[DEBUG] No message from STDIN (EAGAIN).\n");
                    break;  // buffer is full, try again later)
                }
                perror("read");
                exit(1);
            }
        }

    }

    close(sockfd);

    return 0;
}