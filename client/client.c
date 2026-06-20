/* client.c - Cliente P2P principal */

#include "client.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../server/hash.h"

static int parse_port(const char *text, uint16_t *out) {
    unsigned long value;
    char *end = NULL;

    if (text == NULL || out == NULL) {
        return -1;
    }

    value = strtoul(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value == 0 || value > 65535UL) {
        return -1;
    }

    *out = (uint16_t)value;
    return 0;
}

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

static int join_path(char *out, size_t out_size, const char *dir, const char *name) {
    int written;

    written = snprintf(out, out_size, "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

static int scan_shared_folder(p2p_client_context_t *context) {
    DIR *dir;
    struct dirent *entry;

    context->file_count = 0;
    dir = opendir(context->shared_folder);
    if (dir == NULL) {
        fprintf(stderr, "warning: cannot open shared folder '%s': %s\n",
                context->shared_folder, strerror(errno));
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[P2P_MAX_PATH];
        struct stat st;
        p2p_file_metadata_t *file;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (context->file_count >= CLIENT_MAX_LOCAL_FILES) {
            fprintf(stderr, "warning: local file limit reached; skipping remaining files\n");
            break;
        }

        if (strchr(entry->d_name, '\n') != NULL || strlen(entry->d_name) >= P2P_MAX_NAME) {
            fprintf(stderr, "warning: skipping unsupported file name '%s'\n", entry->d_name);
            continue;
        }

        if (join_path(path, sizeof(path), context->shared_folder, entry->d_name) != 0) {
            fprintf(stderr, "warning: skipping path that is too long: %s\n", entry->d_name);
            continue;
        }

        if (stat(path, &st) != 0) {
            fprintf(stderr, "warning: cannot stat '%s': %s\n", path, strerror(errno));
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        file = &context->files[context->file_count];
        memset(file, 0, sizeof(*file));
        snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
        file->size = (uint64_t)st.st_size;

        if (p2p_hash_file(path, file->hash) != 0) {
            fprintf(stderr, "warning: cannot hash '%s': %s\n", path, strerror(errno));
            continue;
        }

        context->file_count++;
    }

    if (closedir(dir) != 0) {
        fprintf(stderr, "warning: cannot close shared folder '%s': %s\n",
                context->shared_folder, strerror(errno));
    }

    return 0;
}

static int connect_to_server(const char *host, uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *it;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        return -1;
    }

    for (it = results; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(results);
    return fd;
}

static int parse_neighbors(int fd, p2p_client_context_t *context) {
    char line[P2P_MAX_LINE];
    unsigned int expected;

    if (read_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "server closed before NEIGHBORS response\n");
        return -1;
    }

    if (sscanf(line, "NEIGHBORS %u", &expected) != 1) {
        fprintf(stderr, "unexpected server response: %s\n", line);
        return -1;
    }

    context->neighbor_count = 0;
    while (read_line(fd, line, sizeof(line)) > 0) {
        char ip[46];
        unsigned int port;

        if (strcmp(line, "END") == 0) {
            return 0;
        }

        if (sscanf(line, "NEIGHBOR %45s %u", ip, &port) == 2 &&
            port > 0 && port <= 65535U &&
            context->neighbor_count < P2P_DEFAULT_NEIGHBORS) {
            p2p_endpoint_t *neighbor = &context->neighbors[context->neighbor_count++];
            snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
            neighbor->port = (uint16_t)port;
        }
    }

    return -1;
}

static int register_with_server(p2p_client_context_t *context) {
    int fd;
    char line[P2P_MAX_LINE];
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = sizeof(local_addr);

    fd = connect_to_server(context->server_ip, context->server_port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to server %s:%u\n",
                context->server_ip, context->server_port);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
        void *address = NULL;
        if (local_addr.ss_family == AF_INET) {
            address = &((struct sockaddr_in *)&local_addr)->sin_addr;
        } else if (local_addr.ss_family == AF_INET6) {
            address = &((struct sockaddr_in6 *)&local_addr)->sin6_addr;
        }
        if (address != NULL) {
            inet_ntop(local_addr.ss_family, address,
                      context->local_ip, sizeof(context->local_ip));
        }
    }


    snprintf(line, sizeof(line), "REGISTER %u %zu\n",
             context->transfer_port, context->file_count);
    if (write_all(fd, line) != 0) {
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < context->file_count; i++) {
        snprintf(line, sizeof(line), "FILE %llu %s %s\n",
                 (unsigned long long)context->files[i].size,
                 context->files[i].hash,
                 context->files[i].name);
        if (write_all(fd, line) != 0) {
            close(fd);
            return -1;
        }
    }

    if (write_all(fd, "END\n") != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "server closed before registration response\n");
        close(fd);
        return -1;
    }

    if (strncmp(line, "OK ", 3) != 0) {
        fprintf(stderr, "registration failed: %s\n", line);
        close(fd);
        return -1;
    }

    if (parse_neighbors(fd, context) != 0) {
        close(fd);
        return -1;
    }

    close(fd);
    printf("registered %zu files with %s:%u\n",
           context->file_count, context->server_ip, context->server_port);
    printf("received %zu neighbors\n", context->neighbor_count);
    return 0;
}

int client_find_on_server(const p2p_client_context_t *context,
                          const char *name,
                          p2p_endpoint_t *peers,
                          size_t max_peers,
                          size_t *peer_count) {
    int fd;
    char line[P2P_MAX_LINE];
    unsigned int expected;

    if (context == NULL || name == NULL || peers == NULL || peer_count == NULL ||
        name[0] == '\0' || strchr(name, '\n') != NULL) {
        return -1;
    }

    *peer_count = 0;
    fd = connect_to_server(context->server_ip, context->server_port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to server %s:%u\n",
                context->server_ip, context->server_port);
        return -1;
    }

    snprintf(line, sizeof(line), "FIND %s\n", name);
    if (write_all(fd, line) != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "server closed before FIND response\n");
        close(fd);
        return -1;
    }

    if (sscanf(line, "PEERS %u", &expected) != 1) {
        fprintf(stderr, "unexpected FIND response: %s\n", line);
        close(fd);
        return -1;
    }

    while (read_line(fd, line, sizeof(line)) > 0) {
        char ip[46];
        unsigned int port;

        if (strcmp(line, "END") == 0) {
            close(fd);
            return 0;
        }

        if (sscanf(line, "PEER %45s %u", ip, &port) == 2 &&
            port > 0 && port <= 65535U &&
            *peer_count < max_peers) {
            p2p_endpoint_t *peer = &peers[*peer_count];
            snprintf(peer->ip, sizeof(peer->ip), "%s", ip);
            peer->port = (uint16_t)port;
            (*peer_count)++;
        }
    }

    close(fd);
    return -1;
}

int client_lookup_on_server(const p2p_client_context_t *context,
                            uint64_t size,
                            const char *hash,
                            p2p_endpoint_t *peers,
                            size_t max_peers,
                            size_t *peer_count) {
    int fd;
    char line[P2P_MAX_LINE];
    unsigned int expected;

    if (context == NULL || hash == NULL || peers == NULL || peer_count == NULL ||
        strlen(hash) != P2P_HASH_HEX_LEN || strchr(hash, '\n') != NULL) {
        return -1;
    }

    *peer_count = 0;
    fd = connect_to_server(context->server_ip, context->server_port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to server %s:%u\n",
                context->server_ip, context->server_port);
        return -1;
    }

    snprintf(line, sizeof(line), "LOOKUP %llu %s\n", (unsigned long long)size, hash);
    if (write_all(fd, line) != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "server closed before LOOKUP response\n");
        close(fd);
        return -1;
    }

    if (sscanf(line, "PEERS %u", &expected) != 1) {
        fprintf(stderr, "unexpected LOOKUP response: %s\n", line);
        close(fd);
        return -1;
    }

    while (read_line(fd, line, sizeof(line)) > 0) {
        char ip[46];
        unsigned int port;

        if (strcmp(line, "END") == 0) {
            close(fd);
            return 0;
        }

        if (sscanf(line, "PEER %45s %u", ip, &port) == 2 &&
            port > 0 && port <= 65535U &&
            *peer_count < max_peers) {
            p2p_endpoint_t *peer = &peers[*peer_count];
            snprintf(peer->ip, sizeof(peer->ip), "%s", ip);
            peer->port = (uint16_t)port;
            (*peer_count)++;
        }
    }

    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    p2p_client_context_t context;

    if (argc != 5) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <transfer_port> <shared_folder>\n",
                argv[0]);
        return 2;
    }

    memset(&context, 0, sizeof(context));
    snprintf(context.server_ip, sizeof(context.server_ip), "%s", argv[1]);
    snprintf(context.shared_folder, sizeof(context.shared_folder), "%s", argv[4]);

    if (parse_port(argv[2], &context.server_port) != 0) {
        fprintf(stderr, "invalid server port: %s\n", argv[2]);
        return 2;
    }

    if (parse_port(argv[3], &context.transfer_port) != 0) {
        fprintf(stderr, "invalid transfer port: %s\n", argv[3]);
        return 2;
    }

    scan_shared_folder(&context);

    if (register_with_server(&context) != 0) {
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (transfer_server_start(&context) != 0) {
        fprintf(stderr, "cannot start transfer server on port %u\n", context.transfer_port);
        return 1;
    }

    client_run_console(&context);
    return 0;
}
