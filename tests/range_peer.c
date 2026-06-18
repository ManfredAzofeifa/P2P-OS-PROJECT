#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../protocol/protocol.h"

#define RANGE_BUFFER_SIZE 4096

static void trim_newline(char *line) {
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int read_line(int fd, char *buffer, size_t size) {
    size_t used = 0;

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
        ssize_t written = send(fd, bytes + total, len - total, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)written;
    }
    return 0;
}

static int write_text(int fd, const char *text) {
    return write_all(fd, text, strlen(text));
}

static int serve_range(int fd, const char *path) {
    char line[P2P_MAX_LINE];
    char header[P2P_MAX_LINE];
    char hash[P2P_HASH_STR_LEN];
    unsigned long long size;
    unsigned long long offset;
    unsigned long long length;
    unsigned char buffer[RANGE_BUFFER_SIZE];
    struct stat st;
    uint64_t remaining;
    FILE *input;

    if (read_line(fd, line, sizeof(line)) <= 0) {
        return -1;
    }
    if (sscanf(line, "GET %llu %32s %llu %llu", &size, hash, &offset, &length) != 4 ||
        strlen(hash) != P2P_HASH_HEX_LEN) {
        return write_text(fd, "ERROR invalid GET\n");
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != (uint64_t)size ||
        (uint64_t)offset > (uint64_t)size || (uint64_t)length > (uint64_t)size - (uint64_t)offset) {
        return write_text(fd, "ERROR invalid range\n");
    }

    input = fopen(path, "rb");
    if (input == NULL) {
        return write_text(fd, "ERROR file unavailable\n");
    }
    if (fseeko(input, (off_t)offset, SEEK_SET) != 0) {
        fclose(input);
        return write_text(fd, "ERROR seek failed\n");
    }

    snprintf(header, sizeof(header), "DATA %llu\n", length);
    if (write_text(fd, header) != 0) {
        fclose(input);
        return -1;
    }

    remaining = (uint64_t)length;
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

int main(int argc, char **argv) {
    unsigned long port;
    unsigned long max_requests;
    char *end = NULL;
    int listen_fd;
    int opt = 1;
    struct sockaddr_in addr;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <file> <max_requests>\n", argv[0]);
        return 2;
    }

    port = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port == 0 || port > 65535UL) {
        fprintf(stderr, "invalid port\n");
        return 2;
    }
    end = NULL;
    max_requests = strtoul(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || max_requests == 0) {
        fprintf(stderr, "invalid max_requests\n");
        return 2;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 8) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("range peer listening on port %lu\n", port);
    fflush(stdout);

    for (unsigned long i = 0; i < max_requests; i++) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                i--;
                continue;
            }
            perror("accept");
            close(listen_fd);
            return 1;
        }
        serve_range(client_fd, argv[2]);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
