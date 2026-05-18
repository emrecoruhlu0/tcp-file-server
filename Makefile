CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0) -lpthread

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

clean:
	rm -f server client server.log

.PHONY: all clean
