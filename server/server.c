/* server.c - Servidor P2P principal */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../protocol/protocol.h"
#include "metadata.h"

#define SERVER_BACKLOG 32
#define MAX_REGISTERED_FILES 4096
#define MAX_REGISTERED_CLIENTS 256

typedef struct {
    int fd;
    struct sockaddr_storage addr;
} client_connection_t;

static p2p_file_metadata_t g_files[MAX_REGISTERED_FILES];
static size_t g_file_count = 0;
static p2p_endpoint_t g_clients[MAX_REGISTERED_CLIENTS];
static size_t g_client_count = 0;
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static void trim_newline(char *line) {
    size_t len;

    if (line == NULL) {
        return;
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int read_line(int fd, char *buffer, size_t size) {
    size_t used = 0;

    if (buffer == NULL || size == 0) {
        return -1;
    }

    while (used + 1 < size) {
        char ch;
        ssize_t nread = recv(fd, &ch, 1, 0);

        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        buffer[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[used] = '\0';
    trim_newline(buffer);
    return used > 0 ? 1 : 0;
}

static int write_all(int fd, const char *text) {
    size_t total = 0;
    size_t len = strlen(text);

    while (total < len) {
        ssize_t nwritten = send(fd, text + total, len - total, 0);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)nwritten;
    }

    return 0;
}

static void endpoint_from_sockaddr(const struct sockaddr_storage *addr,
                                   uint16_t transfer_port,
                                   p2p_endpoint_t *endpoint) {
    const void *src = NULL;

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->port = transfer_port;

    if (addr->ss_family == AF_INET) {
        src = &((const struct sockaddr_in *)addr)->sin_addr;
    } else if (addr->ss_family == AF_INET6) {
        src = &((const struct sockaddr_in6 *)addr)->sin6_addr;
    }

    if (src == NULL ||
        inet_ntop(addr->ss_family, src, endpoint->ip, sizeof(endpoint->ip)) == NULL) {
        snprintf(endpoint->ip, sizeof(endpoint->ip), "unknown");
    }
}

static int same_endpoint(const p2p_endpoint_t *a, const p2p_endpoint_t *b) {
    return a->port == b->port && strcmp(a->ip, b->ip) == 0;
}

static void remove_files_for_owner(const p2p_endpoint_t *owner) {
    size_t i = 0;

    while (i < g_file_count) {
        if (same_endpoint(&g_files[i].owner, owner)) {
            g_files[i] = g_files[g_file_count - 1];
            g_file_count--;
        } else {
            i++;
        }
    }
}

static void upsert_recent_client(const p2p_endpoint_t *endpoint) {
    size_t i;

    for (i = 0; i < g_client_count; i++) {
        if (same_endpoint(&g_clients[i], endpoint)) {
            size_t j;
            p2p_endpoint_t existing = g_clients[i];

            for (j = i; j + 1 < g_client_count; j++) {
                g_clients[j] = g_clients[j + 1];
            }
            g_clients[g_client_count - 1] = existing;
            return;
        }
    }

    if (g_client_count < MAX_REGISTERED_CLIENTS) {
        g_clients[g_client_count++] = *endpoint;
        return;
    }

    memmove(&g_clients[0], &g_clients[1], sizeof(g_clients[0]) * (MAX_REGISTERED_CLIENTS - 1));
    g_clients[MAX_REGISTERED_CLIENTS - 1] = *endpoint;
}

static size_t collect_neighbors(const p2p_endpoint_t *self,
                                p2p_endpoint_t *neighbors,
                                size_t max_neighbors) {
    size_t count = 0;
    size_t remaining = g_client_count;

    while (remaining > 0 && count < max_neighbors) {
        const p2p_endpoint_t *candidate = &g_clients[remaining - 1];
        if (!same_endpoint(candidate, self)) {
            neighbors[count++] = *candidate;
        }
        remaining--;
    }

    return count;
}

static void handle_register(int fd,
                            const struct sockaddr_storage *addr,
                            const char *line) {
    unsigned int transfer_port;
    unsigned int expected_files;
    p2p_endpoint_t owner;
    p2p_file_metadata_t pending[MAX_REGISTERED_FILES];
    size_t pending_count = 0;
    p2p_endpoint_t neighbors[P2P_DEFAULT_NEIGHBORS];
    size_t neighbor_count;
    char response[P2P_MAX_LINE];

    if (sscanf(line, "REGISTER %u %u", &transfer_port, &expected_files) != 2 ||
        transfer_port > 65535U) {
        write_all(fd, "ERROR invalid REGISTER\n");
        return;
    }

    endpoint_from_sockaddr(addr, (uint16_t)transfer_port, &owner);

    while (pending_count < MAX_REGISTERED_FILES) {
        char file_line[P2P_MAX_LINE];
        int status = read_line(fd, file_line, sizeof(file_line));
        unsigned long long size;
        char hash[P2P_HASH_STR_LEN];
        char name[P2P_MAX_NAME];

        if (status <= 0) {
            write_all(fd, "ERROR incomplete REGISTER\n");
            return;
        }
        if (strcmp(file_line, "END") == 0) {
            break;
        }

        if (sscanf(file_line, "FILE %llu %32s %255[^\n]", &size, hash, name) != 3) {
            write_all(fd, "ERROR invalid FILE\n");
            return;
        }

        memset(&pending[pending_count], 0, sizeof(pending[pending_count]));
        snprintf(pending[pending_count].name, sizeof(pending[pending_count].name), "%s", name);
        snprintf(pending[pending_count].hash, sizeof(pending[pending_count].hash), "%s", hash);
        pending[pending_count].size = (uint64_t)size;
        pending[pending_count].owner = owner;
        pending_count++;
    }

    pthread_mutex_lock(&g_registry_mutex);
    neighbor_count = collect_neighbors(&owner, neighbors, P2P_DEFAULT_NEIGHBORS);
    remove_files_for_owner(&owner);
    upsert_recent_client(&owner);

    for (size_t i = 0; i < pending_count && g_file_count < MAX_REGISTERED_FILES; i++) {
        g_files[g_file_count++] = pending[i];
    }
    pthread_mutex_unlock(&g_registry_mutex);

    (void)expected_files;

    snprintf(response, sizeof(response), "OK registered %zu files\n", pending_count);
    write_all(fd, response);
    snprintf(response, sizeof(response), "NEIGHBORS %zu\n", neighbor_count);
    write_all(fd, response);
    for (size_t i = 0; i < neighbor_count; i++) {
        snprintf(response, sizeof(response), "NEIGHBOR %s %u\n",
                 neighbors[i].ip, neighbors[i].port);
        write_all(fd, response);
    }
    write_all(fd, "END\n");
}

static void handle_find(int fd, const char *line) {
    char name[P2P_MAX_NAME];
    p2p_endpoint_t peers[P2P_MAX_PEERS];
    size_t peer_count = 0;
    char response[P2P_MAX_LINE];

    if (sscanf(line, "FIND %255[^\n]", name) != 1) {
        write_all(fd, "ERROR invalid FIND\n");
        return;
    }

    pthread_mutex_lock(&g_registry_mutex);
    for (size_t i = 0; i < g_file_count && peer_count < P2P_MAX_PEERS; i++) {
        if (strcmp(g_files[i].name, name) == 0) {
            int duplicate = 0;
            for (size_t j = 0; j < peer_count; j++) {
                if (same_endpoint(&peers[j], &g_files[i].owner)) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                peers[peer_count++] = g_files[i].owner;
            }
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);

    snprintf(response, sizeof(response), "PEERS %zu\n", peer_count);
    write_all(fd, response);
    for (size_t i = 0; i < peer_count; i++) {
        snprintf(response, sizeof(response), "PEER %s %u\n", peers[i].ip, peers[i].port);
        write_all(fd, response);
    }
    write_all(fd, "END\n");
}

static void handle_lookup(int fd, const char *line) {
    unsigned long long size;
    char hash[P2P_HASH_STR_LEN];
    p2p_endpoint_t peers[P2P_MAX_PEERS];
    size_t peer_count = 0;
    char response[P2P_MAX_LINE];

    if (sscanf(line, "LOOKUP %llu %32s", &size, hash) != 2) {
        write_all(fd, "ERROR invalid LOOKUP\n");
        return;
    }

    pthread_mutex_lock(&g_registry_mutex);
    for (size_t i = 0; i < g_file_count && peer_count < P2P_MAX_PEERS; i++) {
        if (g_files[i].size == (uint64_t)size && strcmp(g_files[i].hash, hash) == 0) {
            int duplicate = 0;
            for (size_t j = 0; j < peer_count; j++) {
                if (same_endpoint(&peers[j], &g_files[i].owner)) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                peers[peer_count++] = g_files[i].owner;
            }
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);

    snprintf(response, sizeof(response), "PEERS %zu\n", peer_count);
    write_all(fd, response);
    for (size_t i = 0; i < peer_count; i++) {
        snprintf(response, sizeof(response), "PEER %s %u\n", peers[i].ip, peers[i].port);
        write_all(fd, response);
    }
    write_all(fd, "END\n");
}

static void *client_thread(void *arg) {
    client_connection_t *connection = arg;
    char line[P2P_MAX_LINE];

    if (read_line(connection->fd, line, sizeof(line)) > 0) {
        if (strncmp(line, "REGISTER ", 9) == 0) {
            handle_register(connection->fd, &connection->addr, line);
        } else if (strncmp(line, "FIND ", 5) == 0) {
            handle_find(connection->fd, line);
        } else if (strncmp(line, "LOOKUP ", 7) == 0) {
            handle_lookup(connection->fd, line);
        } else {
            write_all(connection->fd, "ERROR unknown command\n");
        }
    }

    close(connection->fd);
    free(connection);
    return NULL;
}

static int create_listen_socket(uint16_t port) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SERVER_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv) {
    unsigned long port_value;
    char *end = NULL;
    int listen_fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }

    port_value = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port_value == 0 || port_value > 65535UL) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);

    listen_fd = create_listen_socket((uint16_t)port_value);
    if (listen_fd < 0) {
        perror("listen socket");
        return 1;
    }

    printf("P2P server listening on port %lu\n", port_value);
    fflush(stdout);

    while (1) {
        client_connection_t *connection;
        socklen_t addr_len;
        pthread_t thread;

        connection = calloc(1, sizeof(*connection));
        if (connection == NULL) {
            continue;
        }

        addr_len = sizeof(connection->addr);
        connection->fd = accept(listen_fd, (struct sockaddr *)&connection->addr, &addr_len);
        if (connection->fd < 0) {
            free(connection);
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        if (pthread_create(&thread, NULL, client_thread, connection) != 0) {
            close(connection->fd);
            free(connection);
            continue;
        }
        pthread_detach(thread);
    }

    close(listen_fd);
    return 1;
}
