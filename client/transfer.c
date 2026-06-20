/* transfer.c - Envio y recepcion de archivos por segmentos */

#include "transfer.h"
#include "../distributed/discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../server/hash.h"

#define TRANSFER_BACKLOG 16
#define TRANSFER_BUFFER_SIZE 8192

typedef struct {
    int fd;
    const p2p_client_context_t *context;
} transfer_request_t;

typedef struct {
    int listen_fd;
    const p2p_client_context_t *context;
} transfer_server_t;

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

static int write_all(int fd, const void *data, size_t len) {
    const char *bytes = data;
    size_t total = 0;

    while (total < len) {
        ssize_t nwritten = send(fd, bytes + total, len - total, 0);

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

static int write_text(int fd, const char *text) {
    return write_all(fd, text, strlen(text));
}

static int join_path(char *out, size_t out_size, const char *dir, const char *name) {
    int written = snprintf(out, out_size, "%s/%s", dir, name);

    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

static const p2p_file_metadata_t *find_local_file(const p2p_client_context_t *context,
                                                  uint64_t size,
                                                  const char *hash) {
    for (size_t i = 0; i < context->file_count; i++) {
        const p2p_file_metadata_t *file = &context->files[i];
        if (file->size == size && strcmp(file->hash, hash) == 0) {
            return file;
        }
    }

    return NULL;
}

static int send_file_range(int fd,
                           const p2p_client_context_t *context,
                           const p2p_file_metadata_t *file,
                           uint64_t offset,
                           uint64_t length) {
    char path[P2P_MAX_PATH];
    char header[P2P_MAX_LINE];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    FILE *input;
    struct stat st;
    uint64_t remaining = length;

    if (offset > file->size || length > file->size - offset) {
        return write_text(fd, "ERROR invalid range\n");
    }

    if (join_path(path, sizeof(path), context->shared_folder, file->name) != 0) {
        return write_text(fd, "ERROR path too long\n");
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != file->size) {
        return write_text(fd, "ERROR file unavailable\n");
    }

    input = fopen(path, "rb");
    if (input == NULL) {
        return write_text(fd, "ERROR file unavailable\n");
    }

    if (fseeko(input, (off_t)offset, SEEK_SET) != 0) {
        fclose(input);
        return write_text(fd, "ERROR seek failed\n");
    }

    snprintf(header, sizeof(header), "DATA %llu\n", (unsigned long long)length);
    if (write_text(fd, header) != 0) {
        fclose(input);
        return -1;
    }

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        size_t nread = fread(buffer, 1, chunk, input);

        if (nread == 0) {
            fclose(input);
            return -1;
        }

        if (write_all(fd, buffer, nread) != 0) {
            fclose(input);
            return -1;
        }

        remaining -= nread;
    }

    fclose(input);
    return 0;
}

static void *transfer_request_thread(void *arg) {
    transfer_request_t *request = arg;
    char line[P2P_MAX_LINE];
    unsigned long long size;
    unsigned long long offset;
    unsigned long long length;
    char hash[P2P_HASH_STR_LEN];
    const p2p_file_metadata_t *file;

    if (read_line(request->fd, line, sizeof(line)) <= 0) {
        close(request->fd);
        free(request);
        return NULL;
    }
    if (distributed_handle_peer_message(request->context, line)) {
        close(request->fd);
        free(request);
        return NULL;
    }


    if (sscanf(line, "GET %llu %32s %llu %llu", &size, hash, &offset, &length) != 4 ||
        strlen(hash) != P2P_HASH_HEX_LEN) {
        write_text(request->fd, "ERROR invalid GET\n");
        close(request->fd);
        free(request);
        return NULL;
    }

    file = find_local_file(request->context, (uint64_t)size, hash);
    if (file == NULL) {
        write_text(request->fd, "ERROR file not found\n");
        close(request->fd);
        free(request);
        return NULL;
    }

    send_file_range(request->fd, request->context, file, (uint64_t)offset, (uint64_t)length);
    close(request->fd);
    free(request);
    return NULL;
}

static void *transfer_accept_thread(void *arg) {
    transfer_server_t *server = arg;

    while (1) {
        transfer_request_t *request;
        pthread_t thread;
        int fd = accept(server->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        request = calloc(1, sizeof(*request));
        if (request == NULL) {
            close(fd);
            continue;
        }

        request->fd = fd;
        request->context = server->context;

        if (pthread_create(&thread, NULL, transfer_request_thread, request) != 0) {
            close(fd);
            free(request);
            continue;
        }
        pthread_detach(thread);
    }

    close(server->listen_fd);
    free(server);
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

    if (listen(fd, TRANSFER_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int read_exact(int fd, unsigned char *buffer, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t nread = recv(fd, buffer + total, len - total, 0);

        if (nread == 0) {
            return -1;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)nread;
    }

    return 0;
}

static int connect_to_peer(const p2p_endpoint_t *peer) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *it;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", peer->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(peer->ip, port_text, &hints, &results) != 0) {
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

static int file_matches_request(const char *path, uint64_t size, const char *hash) {
    struct stat st;
    char actual_hash[P2P_HASH_STR_LEN];

    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != size) {
        return -1;
    }
    if (p2p_hash_file(path, actual_hash) != 0) {
        return -1;
    }
    return strcmp(actual_hash, hash) == 0 ? 1 : -1;
}

static int build_partial_path(char *out, size_t out_size, const char *path) {
    int written = snprintf(out, out_size, "%s.part", path);

    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

int transfer_download_from_peer(const p2p_client_context_t *context,
                                const p2p_endpoint_t *peer,
                                uint64_t size,
                                const char *hash,
                                char *saved_path,
                                size_t saved_path_size,
                                int *already_present) {
    int fd;
    char request[P2P_MAX_LINE];
    char line[P2P_MAX_LINE];
    char path[P2P_MAX_PATH];
    char partial_path[P2P_MAX_PATH];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    unsigned long long data_length;
    uint64_t remaining = size;
    FILE *output;
    int existing_status;

    if (already_present != NULL) {
        *already_present = 0;
    }
    if (context == NULL || peer == NULL || hash == NULL || strlen(hash) != P2P_HASH_HEX_LEN) {
        return -1;
    }
    if (join_path(path, sizeof(path), context->shared_folder, hash) != 0 ||
        build_partial_path(partial_path, sizeof(partial_path), path) != 0) {
        return -1;
    }

    existing_status = file_matches_request(path, size, hash);
    if (existing_status > 0) {
        if (saved_path != NULL && saved_path_size > 0) {
            snprintf(saved_path, saved_path_size, "%s", path);
        }
        if (already_present != NULL) {
            *already_present = 1;
        }
        return 0;
    }
    if (existing_status < 0) {
        return -1;
    }

    fd = connect_to_peer(peer);
    if (fd < 0) {
        return -1;
    }

    snprintf(request, sizeof(request), "GET %llu %s 0 %llu\n",
             (unsigned long long)size, hash, (unsigned long long)size);
    if (write_text(fd, request) != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, line, sizeof(line)) <= 0) {
        close(fd);
        return -1;
    }

    if (strncmp(line, "ERROR ", 6) == 0) {
        fprintf(stderr, "peer %s:%u returned %s\n", peer->ip, peer->port, line);
        close(fd);
        return -1;
    }
    if (sscanf(line, "DATA %llu", &data_length) != 1 || data_length != size) {
        close(fd);
        return -1;
    }

    unlink(partial_path);
    output = fopen(partial_path, "wb");
    if (output == NULL) {
        close(fd);
        return -1;
    }

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;

        if (read_exact(fd, buffer, chunk) != 0) {
            fclose(output);
            close(fd);
            unlink(partial_path);
            return -1;
        }
        if (fwrite(buffer, 1, chunk, output) != chunk) {
            fclose(output);
            close(fd);
            unlink(partial_path);
            return -1;
        }
        remaining -= chunk;
    }

    if (fclose(output) != 0) {
        close(fd);
        unlink(partial_path);
        return -1;
    }
    close(fd);

    if (file_matches_request(partial_path, size, hash) <= 0) {
        unlink(partial_path);
        return -1;
    }
    if (link(partial_path, path) != 0) {
        unlink(partial_path);
        return -1;
    }
    unlink(partial_path);

    if (saved_path != NULL && saved_path_size > 0) {
        snprintf(saved_path, saved_path_size, "%s", path);
    }
    return 0;
}

typedef struct {
    const p2p_client_context_t *context;
    const p2p_endpoint_t *peer;
    uint64_t size;
    const char *hash;
    const char *partial_path;
    uint64_t offset;
    uint64_t length;
    int result;
} range_download_t;

static int create_empty_file(const char *path, uint64_t size) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);

    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

static int download_range_to_path(const p2p_endpoint_t *peer,
                                  uint64_t size,
                                  const char *hash,
                                  uint64_t offset,
                                  uint64_t length,
                                  const char *partial_path) {
    int fd;
    char request[P2P_MAX_LINE];
    char line[P2P_MAX_LINE];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    unsigned long long data_length;
    uint64_t remaining = length;
    FILE *output;

    fd = connect_to_peer(peer);
    if (fd < 0) {
        return -1;
    }

    snprintf(request, sizeof(request), "GET %llu %s %llu %llu\n",
             (unsigned long long)size, hash,
             (unsigned long long)offset, (unsigned long long)length);
    if (write_text(fd, request) != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, line, sizeof(line)) <= 0) {
        close(fd);
        return -1;
    }
    if (strncmp(line, "ERROR ", 6) == 0) {
        fprintf(stderr, "peer %s:%u returned %s\n", peer->ip, peer->port, line);
        close(fd);
        return -1;
    }
    if (sscanf(line, "DATA %llu", &data_length) != 1 || data_length != length) {
        close(fd);
        return -1;
    }

    output = fopen(partial_path, "r+b");
    if (output == NULL) {
        close(fd);
        return -1;
    }
    if (fseeko(output, (off_t)offset, SEEK_SET) != 0) {
        fclose(output);
        close(fd);
        return -1;
    }

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;

        if (read_exact(fd, buffer, chunk) != 0) {
            fclose(output);
            close(fd);
            return -1;
        }
        if (fwrite(buffer, 1, chunk, output) != chunk) {
            fclose(output);
            close(fd);
            return -1;
        }
        remaining -= chunk;
    }

    if (fclose(output) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void *range_download_thread(void *arg) {
    range_download_t *range = arg;

    range->result = download_range_to_path(range->peer, range->size, range->hash,
                                           range->offset, range->length,
                                           range->partial_path);
    return NULL;
}

static int install_partial_file(const char *partial_path,
                                const char *path,
                                uint64_t size,
                                const char *hash) {
    if (file_matches_request(partial_path, size, hash) <= 0) {
        unlink(partial_path);
        return -1;
    }
    if (link(partial_path, path) != 0) {
        unlink(partial_path);
        return -1;
    }
    unlink(partial_path);
    return 0;
}

int transfer_download_from_peers(const p2p_client_context_t *context,
                                 const p2p_endpoint_t *peers,
                                 size_t peer_count,
                                 uint64_t size,
                                 const char *hash,
                                 char *saved_path,
                                 size_t saved_path_size,
                                 int *already_present,
                                 int *segmented) {
    char path[P2P_MAX_PATH];
    char partial_path[P2P_MAX_PATH];
    range_download_t ranges[P2P_MAX_PEERS];
    pthread_t threads[P2P_MAX_PEERS];
    size_t range_count;
    uint64_t offset = 0;
    uint64_t base;
    uint64_t extra;
    int existing_status;
    int failed = 0;

    if (already_present != NULL) {
        *already_present = 0;
    }
    if (segmented != NULL) {
        *segmented = 0;
    }
    if (context == NULL || peers == NULL || peer_count == 0 ||
        hash == NULL || strlen(hash) != P2P_HASH_HEX_LEN) {
        return -1;
    }

    if (peer_count == 1 || size == 0) {
        return transfer_download_from_peer(context, &peers[0], size, hash,
                                           saved_path, saved_path_size,
                                           already_present);
    }

    if (join_path(path, sizeof(path), context->shared_folder, hash) != 0 ||
        build_partial_path(partial_path, sizeof(partial_path), path) != 0) {
        return -1;
    }

    existing_status = file_matches_request(path, size, hash);
    if (existing_status > 0) {
        if (saved_path != NULL && saved_path_size > 0) {
            snprintf(saved_path, saved_path_size, "%s", path);
        }
        if (already_present != NULL) {
            *already_present = 1;
        }
        return 0;
    }
    if (existing_status < 0) {
        return -1;
    }

    range_count = peer_count > P2P_MAX_PEERS ? P2P_MAX_PEERS : peer_count;
    if ((uint64_t)range_count > size) {
        range_count = (size_t)size;
    }

    unlink(partial_path);
    if (create_empty_file(partial_path, size) != 0) {
        return -1;
    }

    base = size / range_count;
    extra = size % range_count;
    memset(ranges, 0, sizeof(ranges));
    for (size_t i = 0; i < range_count; i++) {
        ranges[i].context = context;
        ranges[i].peer = &peers[i];
        ranges[i].size = size;
        ranges[i].hash = hash;
        ranges[i].partial_path = partial_path;
        ranges[i].offset = offset;
        ranges[i].length = base + (i < extra ? 1 : 0);
        ranges[i].result = -1;
        offset += ranges[i].length;

        if (pthread_create(&threads[i], NULL, range_download_thread, &ranges[i]) != 0) {
            ranges[i].result = -1;
            failed = 1;
            for (size_t j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            break;
        }
    }

    if (!failed) {
        for (size_t i = 0; i < range_count; i++) {
            pthread_join(threads[i], NULL);
            if (ranges[i].result != 0) {
                failed = 1;
            }
        }
    }

    if (failed) {
        for (size_t i = 0; i < range_count; i++) {
            if (ranges[i].result == 0) {
                continue;
            }

            ranges[i].result = -1;
            for (size_t j = 0; j < peer_count && j < P2P_MAX_PEERS; j++) {
                if (&peers[j] == ranges[i].peer) {
                    continue;
                }
                if (download_range_to_path(&peers[j], size, hash,
                                           ranges[i].offset, ranges[i].length,
                                           partial_path) == 0) {
                    ranges[i].result = 0;
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < range_count; i++) {
        if (ranges[i].result != 0) {
            unlink(partial_path);
            return -1;
        }
    }

    if (install_partial_file(partial_path, path, size, hash) != 0) {
        return -1;
    }

    if (saved_path != NULL && saved_path_size > 0) {
        snprintf(saved_path, saved_path_size, "%s", path);
    }
    if (segmented != NULL) {
        *segmented = 1;
    }
    return 0;
}

int transfer_server_start(const p2p_client_context_t *context) {
    transfer_server_t *server;
    pthread_t thread;
    int listen_fd;

    if (context == NULL) {
        return -1;
    }

    listen_fd = create_listen_socket(context->transfer_port);
    if (listen_fd < 0) {
        return -1;
    }

    server = calloc(1, sizeof(*server));
    if (server == NULL) {
        close(listen_fd);
        return -1;
    }

    server->listen_fd = listen_fd;
    server->context = context;

    if (pthread_create(&thread, NULL, transfer_accept_thread, server) != 0) {
        close(listen_fd);
        free(server);
        return -1;
    }

    pthread_detach(thread);
    printf("transfer server listening on port %u\n", context->transfer_port);
    return 0;
}
