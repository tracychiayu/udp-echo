CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS =

DEPS =

all: server client

server: server.o $(DEPS)
	$(CC) $(LDFLAGS) -o server server.o

client: client.o $(DEPS)
	$(CC) $(LDFLAGS) -o client client.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf server client *.bin *.o
