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

int state = 0;           // Current state for handshake
int our_send_window = 0; // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size
int our_max_receiving_window = MIN_WINDOW; // Our max receiving window
int our_recv_window = 0;                   // Bytes in our recv buf
int dup_acks = 0;        // Duplicate acknowledgements received
uint32_t ack = 0;        // Acknowledgement number
uint32_t seq = 0;        // Sequence number
uint32_t temp_seq = 0;   // Save current seq#. Retreive after dup ack packet transmission
uint32_t last_ack = 0;   // Last ACK number to keep track of duplicate ACKs
bool pure_ack = false;   // Require ACK to be sent out
bool syn_sent = false;
bool syn_ack_received = false;
bool dup_acks_retransmission = false;
bool drop_packet = false;
packet* base_pkt = NULL; // Lowest outstanding packet to be sent out

buffer_node* recv_buf = NULL;       // Linked list storing out of order received packets (points to the start of the buffer)
buffer_node* recv_buf_tail = NULL;  // Pointer that points to the tail of the recv_buf
buffer_node* send_buf = NULL;       // Linked list storing packets that were sent but not acknowledged (points to the start of the buffer)
buffer_node* send_buf_tail = NULL;  // Pointer that points to the tail of the send_buf

ssize_t (*input)(uint8_t*, size_t); // Get data from layer
void (*output)(uint8_t*, size_t);   // Output data from layer

struct timeval start; // Last packet sent at this time
struct timeval now;   // Temp for current time

// HELPER FUNCTIONS
void increment_recv_window(){
    if (our_max_receiving_window == MAX_WINDOW){ return; }
    if (our_max_receiving_window + 500 < MAX_WINDOW){
        our_max_receiving_window += 500;
    }
    else{
        our_max_receiving_window = MAX_WINDOW;
    }
}
void insert_send_buffer(packet* pkt){
    int payload_len = ntohs(pkt->length);
    buffer_node* node = calloc(1, sizeof(buffer_node) + payload_len);
    memcpy(&node->pkt, pkt, sizeof(packet) + payload_len);
    node->next = NULL;
    
    if (send_buf == NULL){
        // 1. if send_buf == NULL, insert the first packet
        send_buf = node;
        send_buf_tail = node;
    }
    else {
        // 2. if send_buf != NULL, insert node after the node send_buf_tail is pointing to, and update send_buf_tail
        send_buf_tail->next = node;
        send_buf_tail = send_buf_tail->next;
    }
}

void insert_recv_buffer(packet* pkt){
    int payload_len = ntohs(pkt->length);
    buffer_node* node = calloc(1, sizeof(buffer_node) + payload_len);
    memcpy(&node->pkt, pkt, sizeof(packet) + payload_len);
    node->next = NULL;
    uint16_t new_seq = ntohs(node->pkt.seq);  // seq # of the new recv pkt we want to insert


    if (recv_buf == NULL){  // 1. if recv_buf == NULL, insert as the first packet     
        recv_buf = node;
        our_recv_window += payload_len;
        increment_recv_window();
        return;
    }
    else{   // 2. if recv_buf != NULL
        // edge case: insert new recv pkt at the start of the list
        uint16_t start_seq = ntohs(recv_buf->pkt.seq);
        if (new_seq < start_seq){
            node->next = recv_buf;
            recv_buf = node;
            our_recv_window += payload_len;
            increment_recv_window();
            return;
        }

        buffer_node* traverse_ptr = recv_buf;
        buffer_node* prev_ptr;
        uint16_t curr_seq = ntohs(traverse_ptr->pkt.seq);
        // traverse until we reach end of list or new_seq < curr_seq
        while ((traverse_ptr->next != NULL) && (new_seq > curr_seq)){
            // track with two pointers
            prev_ptr = traverse_ptr;
            traverse_ptr = traverse_ptr->next;
            curr_seq = ntohs(traverse_ptr->pkt.seq);
        }
    
        if (traverse_ptr->next == NULL){
            // a. insert new recv pkt at the end of the list
            traverse_ptr->next = node;
        }
        else if (new_seq < curr_seq){
            // b. insert new recv pkt in the middle of the list
            prev_ptr->next = node;
            node->next = traverse_ptr;
        }
        else if (new_seq == curr_seq){
            // c. do nothing if the recv pkt has already in recv_buf (recv duplicate pkts)
            return;
        }
        our_recv_window += payload_len;
        increment_recv_window();
    }    
}

// Remove packet with SEQ# < ACK# from send buffer
void remove_packets_from_send_buffer(uint32_t ack){
    while ((send_buf != NULL) && (ntohs(send_buf->pkt.seq) < ack)){

        uint16_t pkt_length = ntohs(send_buf->pkt.length);
        our_send_window -= pkt_length;

        fprintf(stderr, "[DEBUG] Remove packet %u from send buffer.\n", ntohs(send_buf->pkt.seq));
        buffer_node* temp = send_buf;
        send_buf = send_buf->next;
        
        free(temp);
    }
}

// Find packet with specific SEQ# in send buffer
packet* find_pkt_in_send_buf(uint16_t seq){
    buffer_node* traverse_ptr = send_buf;
    if (traverse_ptr == NULL){ return NULL;  }
    while (traverse_ptr!= NULL){
        uint16_t curr_seq = ntohs(traverse_ptr->pkt.seq);
        if (curr_seq == seq){
            return &traverse_ptr->pkt;
        }
        else{
            traverse_ptr = traverse_ptr->next;
        }
    }

    // if pkt is not in send_buf, return NULL
    return NULL;
}

// Check if a packet with SEQ# seq is in recv buffer
bool is_in_recv_buf(uint32_t target_seq){

    // Empty buffer
    if (recv_buf == NULL){ return false; }

    buffer_node* traverse_ptr = recv_buf;
    uint16_t curr_seq;
    while (traverse_ptr != NULL){
        curr_seq = ntohs(traverse_ptr->pkt.seq);
        if (curr_seq == target_seq){ return true; }
        traverse_ptr = traverse_ptr->next;
    }
    return false;
}

// Adjust current ACK#: check each packet in recv_buf; 
// 1. If there's a gap in recv_buf, return expected gap SEQ# as the updated ACK#
// 2. If packets are consecutively in-order, return last packet's SEQ# + 1 as the updated ACK#
void adjust_ack(){
    buffer_node* traverse_ptr = recv_buf;
    uint16_t curr_seq;
    uint16_t next_seq;
    if (traverse_ptr == NULL){ return; }
    while (traverse_ptr->next != NULL){
        curr_seq = ntohs(traverse_ptr->pkt.seq);
        next_seq = ntohs(traverse_ptr->next->pkt.seq);
        // Case 1
        if (next_seq != curr_seq + 1){
            ack = curr_seq + 1;
            return;
        }
        traverse_ptr = traverse_ptr->next;
    }
    // Case 2
    ack = ntohs(traverse_ptr->pkt.seq) + 1;
    return;
}

// Scan and write out in order/acked packets in recv_buff
void output_recv_buffer(){
    
    if (recv_buf == NULL){ return; }
    while(recv_buf != NULL){

        uint16_t curr_seq = ntohs(recv_buf->pkt.seq);
        if (curr_seq < ack){
            uint payload_len = ntohs(recv_buf->pkt.length);
            // output(recv_buf->pkt.payload, payload_len);
            fprintf(stderr,"[DEBUG] Output RECV BUF with SEQ# %u\n", ntohs(recv_buf->pkt.seq));
            print_buf(recv_buf, RECV);

            buffer_node* temp = recv_buf;
            recv_buf = recv_buf->next;

            our_recv_window -= payload_len;
            free(temp);
        }
        else{
            break;
        }
    }
}

packet* generate_pure_ack_packet(){
    // respond with pure ACK
    packet* pkt = calloc(1,sizeof(packet));
    pkt->seq = htons(0);
    pkt->ack = htons(ack);
    pkt->length = htons(0); 
    pkt->win = htons(our_max_receiving_window-our_recv_window);  
    pkt->flags = ACK;
    pkt->unused = htons(0);

    fprintf(stderr,"\nPURE ACK:\n");
    print_diag(pkt, SEND);
    fprintf(stderr, "\n");

    return pkt;
}

// Prepare data to send out
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
            pkt->length = htons(0); 
            pkt->win = htons(our_max_receiving_window);  
            pkt->flags = ACK;
            pkt->unused = htons(0);

            state = NORMAL;
            print_diag(pkt, SEND);
            fprintf(stderr, "\n");

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

            state = CLIENT_AWAIT;

            syn_sent = true;  // used so that SYN packet will only be sent once

            print_diag(pkt, SEND);
            fprintf(stderr, "\n");
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

        state = SERVER_AWAIT;

        print_diag(pkt, SEND);
        fprintf(stderr, "\n");
        return pkt;
        break;
    }
    case NORMAL: {

        drop_packet = false;
        // Retransmit dup-acked packet
        if (dup_acks_retransmission){

            // Find pkt in send_buf for retransmission
            packet* original_pkt = find_pkt_in_send_buf(seq);
            if (original_pkt == NULL){ return NULL; } // dup-acked packet cannot be found in send_buf, send nothing

            // Make a deep copy of original packet
            int payload_len = ntohs(original_pkt->length);
            packet* pkt = calloc(1, sizeof(packet) + payload_len);
            memcpy(pkt, original_pkt, sizeof(packet) + payload_len);

            // Modify ACK before sending
            pkt->ack = htons(ack);
            pkt->win = htons(our_max_receiving_window-our_recv_window);  

            fprintf(stderr, "\nFAST RETRANSMIT packet # %hu\n", ntohs(pkt->seq));
            print_diag(pkt, SEND);
            fprintf(stderr, "\n");

            if (temp_seq != 0){ seq = temp_seq; }
            dup_acks_retransmission = false; 
            dup_acks = 0;  // reset

            return pkt;
        }

        // Read data from STDIN only when receiver's window size is greater than MAX_PAYLOAD
        if (their_receiving_window >= MAX_PAYLOAD){
            uint8_t buffer[MAX_PAYLOAD];
            ssize_t bytes_read = input(buffer, MAX_PAYLOAD);
            if (bytes_read == 0){ return NULL; }  // return NULL packet if we have no data (from STDIN) to send yet
            else{ 
                // Generate packet with payload
                packet* pkt = calloc(1,sizeof(packet) + bytes_read);

                seq += 1;
                pkt->seq = htons(seq);
                pkt->ack = htons(ack);
                pkt->length = htons(bytes_read);  
                pkt->win = htons(our_max_receiving_window-our_recv_window);  
                pkt->flags = ACK;
                pkt->unused = htons(0);
                memcpy(pkt->payload, buffer, bytes_read);

                insert_send_buffer(pkt);
                our_send_window += bytes_read;

                // DEBUG: drop pkt
                if (seq == 303 || seq == 307){ 
                    fprintf(stderr, "Dropping pkt %d\n", seq);
                    fprintf(stderr, "\n");
                    drop_packet = true;
                    return NULL; 
                } 

                if (seq == 506 || seq == 510){ 
                    fprintf(stderr, "Dropping pkt %d\n", seq);
                    fprintf(stderr, "\n");
                    drop_packet = true;
                    return NULL; 
                } 
                
                fprintf(stderr, "\n");
                print_diag(pkt, SEND);
                print_buf(send_buf,SEND);

                return pkt;
            }
        }
        else{ return NULL; } // return NULL packet if we don't have enough quota now

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

        uint16_t client_seq = ntohs(pkt->seq);
        uint16_t client_ack = ntohs(pkt->ack);
        ack = client_seq + 1;
        if (pkt->flags == SYN){   // Receive hanshake SYN from client
            state = SERVER_START;
        }
        else if (pkt->flags == ACK){
            last_ack = client_ack;
            their_receiving_window = ntohs(pkt->win);
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
            last_ack = server_ack; // 301

            syn_ack_received = true;
        }
        break;
    }
    case NORMAL: {
        uint16_t their_seq = ntohs(pkt->seq);
        uint16_t their_ack = ntohs(pkt->ack);
        their_receiving_window = htons(pkt->win);
        
        // a. Decide if we need to send a pure ack packet when there's no input later
        // b. Place new packet into recv buffer
        if (their_seq >= ack){ 
            pure_ack = true; 
            insert_recv_buffer(pkt);
            print_buf(recv_buf, RECV);
        }

        // c. Update ACK# for outgoing packet
        if (their_seq == ack){ // we receive what we want
            ack = their_seq + 1;
            if (is_in_recv_buf(ack)){
                adjust_ack();
            }
        }
        else if (their_seq < ack && their_seq != 0){
            // We do not need to process the received packet if its an old packet that we've already acked before
            return;
        }
        // if their_seq == 0, // we receive a pure ACK.
        // if their_seq > ack, we don't need to update ACK# (just leave ack as before)



        // d. Check if we need to retransmit a dup-acked packet. Update/reset SEQ# for outgoing packet if needed
        // fprintf(stderr, "their_ack: %u\n", their_ack);
        // fprintf(stderr, "last_ack: %u\n", last_ack);
        if (their_ack == last_ack){ // Receive dup ack
            dup_acks++;
            fprintf(stderr, "[DEBUG] their_ack == last_ack, dup_acks = %d\n", dup_acks);
            if (dup_acks == DUP_ACKS){ 
                temp_seq = seq;
                seq = their_ack;
                dup_acks_retransmission = true; 
                fprintf(stderr, "dup_acks_retransmission = true\n");
            }
        }
        
        // e. If ACK flag is set, remove packets with SEQ# < received ACK# from send_buf 
        if (pkt->flags == ACK){
            // fprintf(stderr, "[DEBUG] Remove packets with SEQ# < their_ack (%u):\n", their_ack);
            remove_packets_from_send_buffer(their_ack);
            fprintf(stderr, "[DEBUG] Remove packets with SEQ# < %d.\n", their_ack);
            print_buf(send_buf, SEND);
        }

        // f. Linear scan recv_buf and write out acked packets
        output_recv_buffer();
        // adjust_ack();
        last_ack = their_ack;
        // fprintf(stderr, "last ack (at the end): %u\n", last_ack);

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

    struct timeval last_activity;
    struct timeval start;
    gettimeofday(&last_activity, NULL);
    gettimeofday(&start, NULL);

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

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* pkt = (packet*) &buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Start listen loop
    while (true) {
        
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);

        // 1. Receive data from socket
        int bytes_recvd = recvfrom(sockfd, &buffer, sizeof(buffer), 0, (struct sockaddr*) addr, &addr_size);
        // fprintf(stderr, "[DEBUG] Bytes received: %d\n", bytes_recvd);

        if (bytes_recvd > 0) {
            // fprintf(stderr, "[DEBUG] Receiving data from recvfrom()\n");
            print_diag(pkt, RECV);
            fprintf(stderr, "\n");
            recv_data(pkt);
            gettimeofday(&last_activity, NULL);
        }
        // No message received from the server yet; continue listening
        else if (bytes_recvd == -1 && errno != EAGAIN && errno != EWOULDBLOCK){ 
            fprintf(stderr, "[ERROR] recvfrom() failed to receive data from server.\n");
            exit(1);
        }

        // 2. Generate and send data packet
        packet* tosend = get_data();
        // a. Send packet with payload when data is available at STDIN
        if (tosend != NULL) {
            ssize_t sent_bytes = sendto(sockfd, tosend, sizeof(packet) + ntohs(tosend->length), 0, (struct sockaddr*) addr, sizeof(struct sockaddr_in));

            if (sent_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("[ERROR] sendto() failed to send data to socket.\n");
                exit(1);
            }
            free(tosend);
            gettimeofday(&last_activity, NULL);
        }
        // b. Send pure ACK packet when no data is available at STDIN
        else if (pure_ack && !drop_packet) {
            packet* pure_ack_pkt = generate_pure_ack_packet();
            ssize_t sent_bytes = sendto(sockfd, pure_ack_pkt, sizeof(packet), 0, (struct sockaddr*) addr, sizeof(struct sockaddr_in));
            if (sent_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("[ERROR] sendto() failed to send data to socket.\n");
                exit(1);
            }
            free(pure_ack_pkt);
            pure_ack = false;
            gettimeofday(&last_activity, NULL);
        }
        // c. Retransmit remaining packets in send_buf (packets sent out but haven't received ACK from the other end)
        else if (send_buf != NULL){
            gettimeofday(&now, NULL);
            if (TV_DIFF(now, last_activity) > 1000000){  // Ensures that we only retransmit after 1 sec of inactivity
                // Retransmit the first packet in send_buf
                packet* original_pkt = &send_buf->pkt;

                // Make a deep copy of original packet
                int payload_len = ntohs(original_pkt->length);
                packet* pkt = calloc(1, sizeof(packet) + payload_len);
                memcpy(pkt, original_pkt, sizeof(packet) + payload_len);

                // Modify ACK before sending
                pkt->ack = htons(ack);
                pkt->win = htons(our_max_receiving_window-our_recv_window);  

                ssize_t sent_bytes = sendto(sockfd, pkt, sizeof(packet) + ntohs(pkt->length), 0, (struct sockaddr*) addr, sizeof(struct sockaddr_in));
                if (sent_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
                    perror("[ERROR] sendto() failed to send data to socket.\n");
                    exit(1);
                }

                fprintf(stderr, "\nRETRANSMIT packet # %hu to clear up send buffer\n", ntohs(pkt->seq));
                print_diag(pkt, SEND);
                fprintf(stderr, "\n");

                free(pkt);
                gettimeofday(&last_activity, NULL);
            }
        }
        // d. Linear scan recv_buf and write out acked packets
        else if (recv_buf != NULL){
            output_recv_buffer();
        }
        // e. When there's neither input from the socket nor any outgoing packet to send,
        //    we wait for 4 seconds of inactivity before closing the connection,  
        //    to allow time for potential retransmissions or delayed packets to arrive.
        else {
            gettimeofday(&now, NULL);
            if (TV_DIFF(now, last_activity) > 4000000) {  // 4 seconds
                fprintf(stderr, "[INFO] Idle timeout reached. Exiting.\n");
                break;
            }
        }

        usleep(10000);
    }
}