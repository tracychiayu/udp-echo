#include "consts.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

/*
 * the following variables are only informational, 
 * not necessarily required for a valid implementation.
 * feel free to edit/remove.
 */
int state = 0;           // Current state for handshake
int our_send_window = 0; // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size
int our_max_receiving_window = MIN_WINDOW; // Our max receiving window
int our_recv_window = 0;                   // Bytes in our recv buf
int dup_acks = 0;        // Duplicate acknowledgements received
uint32_t ack = 0;        // Acknowledgement number
uint32_t seq = 0;        // Sequence number
uint32_t last_ack = 0;   // Last ACK number to keep track of duplicate ACKs
bool pure_ack = false;  // Require ACK to be sent out
bool syn_sent = false;
bool syn_ack_received = false;
packet* base_pkt = NULL; // Lowest outstanding packet to be sent out

buffer_node* recv_buf = NULL; // Linked list storing out of order received packets
buffer_node* send_buf = NULL; // Linked list storing packets that were sent but not acknowledged

ssize_t (*input)(uint8_t*, size_t); // Get data from layer
void (*output)(uint8_t*, size_t);   // Output data from layer

struct timeval start; // Last packet sent at this time
struct timeval now;   // Temp for current time

// Get data from standard input
packet* get_data() {

    switch (state) {
    case SERVER_AWAIT: {
        break;
    }
    case CLIENT_AWAIT: {
        // Build a ACK to reply for server's SYN-ACK for handshake (3)   
        if (syn_ack_received){
            packet* pkt = calloc(1, sizeof(packet));
            pkt->seq = htons(seq);
            pkt->ack = htons(ack);
            pkt->length = htons(0);   // has to be modified later if there's STDIN on client's side
            pkt->win = htons(our_max_receiving_window);  
            pkt->flags = ACK;
            pkt->unused = htons(0);

            print_diag(pkt, SEND);
            state = NORMAL;

            return pkt; 
        }   
        break;
    }
    case CLIENT_START: {

        // Build a SYN packet for handshake (1)
        if (!syn_sent){ // ensure that SYN packet is only sent once
            packet* pkt = calloc(1, sizeof(packet));
            pkt->seq = htons(seq);
            pkt->ack = htons(0);
            pkt->length = htons(0);
            pkt->win = htons(our_max_receiving_window);
            pkt->flags = SYN;
            pkt->unused = htons(0);

            print_diag(pkt, SEND);
            state = CLIENT_AWAIT;

            syn_sent = true;

            return pkt;
        }
        break;
    }
    case SERVER_START:{

        // Build a SYN-ACK packet for handshake (2)
        packet* pkt = calloc(1, sizeof(packet));
        pkt->seq = htons(seq);
        pkt->ack = htons(ack);
        pkt->length = htons(0);
        pkt->win = htons(our_max_receiving_window);
        pkt->flags = SYN | ACK;
        pkt->unused = htons(0);

        print_diag(pkt, SEND);
        state = SERVER_AWAIT;

        return pkt;
        break;
    }

    default: {
        break;
    }
    }

    return NULL;
}

// Process data received from socket
void recv_data(packet* pkt) {
    
    switch (state) {
    case CLIENT_START: {
        break;
    }
    case SERVER_START: {
        break;
    }
    case SERVER_AWAIT:{

        // Receive hanshake SYN from client
        uint16_t client_seq = ntohs(pkt->seq);
        ack = client_seq + 1;
        if (pkt->flags == SYN){ 
            state = SERVER_START;
        }
        else if (pkt->flags == ACK){
            state = NORMAL;
        }
        break;
    }
    case CLIENT_AWAIT: {
        if (pkt->flags == (SYN | ACK)){
            uint16_t server_seq = ntohs(pkt->seq);
            uint16_t server_ack = ntohs(pkt->ack);
            seq = server_ack;      // 301
            ack = server_seq + 1;  // 501

            syn_ack_received = true;
        }
        break;
    }
    default: {
        break;
    }
    }
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int initial_state,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {
    
    // fprintf(stderr, "[DEBUG] Enter listen loop...\n");

    // Set initial state (whether client or server)
    state = initial_state;

    // Set input and output function pointers
    input = input_p;
    output = output_p;

    // Set socket for nonblocking
    int flags = fcntl(sockfd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, flags);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int) {1}, sizeof(int));

    // Set initial sequence number
    // uint32_t r;
    // int rfd = open("/dev/urandom", 'r');
    // read(rfd, &r, sizeof(uint32_t));
    // close(rfd);
    // srand(r);
    // seq = (rand() % 10) * 100 + 100;
    if (state == CLIENT_START){
        seq = 300;
    }
    else if (state == SERVER_AWAIT){
        seq = 500;
    }

    // Setting timers
    gettimeofday(&now, NULL);
    gettimeofday(&start, NULL);

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* pkt = (packet*) &buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Start listen loop
    int count = 0;
    while (true) {
        count++;
        // fprintf(stderr, "count: %d\n", count);
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);

        // 1. Receive data from socket
        int bytes_recvd = recvfrom(sockfd, &buffer, sizeof(buffer), 0, (struct sockaddr*) addr, &addr_size);
        // fprintf(stderr, "[DEBUG] Bytes received: %d\n", bytes_recvd);

        if (bytes_recvd > 0) {
            // fprintf(stderr, "[DEBUG] Receiving data from recvfrom()\n");
            print_diag(pkt, RECV);
            recv_data(pkt);
        }
        // No message received from the server yet; continue listening
        else if (bytes_recvd == -1 && errno != EAGAIN && errno != EWOULDBLOCK){ 
            fprintf(stderr, "[ERROR] recvfrom() failed to receive data from server.\n");
            exit(1);
        }

        // 2. Generate and send data packet
        packet* tosend = get_data();
        // Data available to send
        if (tosend != NULL) {
            ssize_t sent_bytes = sendto(sockfd, tosend, sizeof(packet) + ntohs(tosend->length), 0, (struct sockaddr*) addr, sizeof(struct sockaddr_in));

            if (sent_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("[ERROR] sendto() failed to send data to socket.\n");
                exit(1);
            }
            free(tosend);
        }
        // Send ACK only packet with no payload (no need to queue in send_buff)
        else if (pure_ack) {
        }

        // Check if timer went off
        gettimeofday(&now, NULL);
        if (TV_DIFF(now, start) >= RTO && base_pkt != NULL) {
        }
        // Duplicate ACKS detected
        else if (dup_acks == DUP_ACKS && base_pkt != NULL) {
        }
        // No data to send, so restart timer
        else if (base_pkt == NULL) {
            gettimeofday(&start, NULL);
        }
    }
}