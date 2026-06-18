/* transfer.c - Envio y recepcion de archivos por segmentos */

#include "transfer.h"

#include <arpa/inet.h>
#include <errno.h>
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
