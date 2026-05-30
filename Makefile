CC = gcc
CFLAGS = -Wall -pthread

all: server client

server: server/server.c server/hash.c
	$(CC) $(CFLAGS) -o bin/server server/server.c server/hash.c

client: client/client.c client/transfer.c client/console.c
	$(CC) $(CFLAGS) -o bin/client client/client.c client/transfer.c client/console.c

clean:
	rm -f bin/server bin/client

.PHONY: all server client clean
