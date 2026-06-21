CC = gcc
CFLAGS = -Wall -Wextra -pthread
BIN_DIR = bin

all: server client

server: server/server.c server/hash.c
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/server server/server.c server/hash.c

client: client/client.c client/client.h client/transfer.c client/console.c distributed/discovery.c server/hash.c
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/client client/client.c client/transfer.c client/console.c distributed/discovery.c server/hash.c

test: test-hash test-server test-client

test-hash:
	bash tests/test_hash.sh

test-server: server
	bash tests/test_server.sh

test-client: server client
	bash tests/test_client_registration.sh
	bash tests/test_multi_client.sh

clean:
	rm -f $(BIN_DIR)/server $(BIN_DIR)/client

.PHONY: all server client test test-hash test-server test-client clean
