#include <stdint.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

void init_io() {
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

ssize_t input_io(uint8_t* buf, size_t max_length) {
    ssize_t len = read(STDIN_FILENO, buf, max_length); 
    
    if (len < 0) {   
        if (errno == EAGAIN || errno == EWOULDBLOCK){ 
            return 0; // no data available at STDIN yet
        }
        fprintf(stderr, "[ERROR] read() failed to read data from STDIN.\n");
        exit(1);
    }
    return len;
}

void output_io(uint8_t* buf, size_t length) {
    write(STDOUT_FILENO, buf, length); 
}
