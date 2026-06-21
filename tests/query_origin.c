#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static uint16_t parse_port(const char *text) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end || !value || value > 65535UL) exit(2);
    return (uint16_t)value;
}

static int write_all(int fd, const char *text) {
    size_t total = 0, length = strlen(text);
    while (total < length) {
        ssize_t n = send(fd, text + total, length - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int send_search(uint16_t target_port, uint16_t origin_port,
                       const char *query_id, const char *term) {
    struct sockaddr_in address;
    char line[1024];
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(target_port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    snprintf(line, sizeof(line), "DSEARCH %s 127.0.0.1 %u 4 %s\n",
             query_id, origin_port, term);
    if (write_all(fd, line) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int read_line(int fd, char *line, size_t size) {
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t n = recv(fd, line + used, 1, 0);
        if (n <= 0) break;
        if (line[used++] == '\n') break;
    }
    line[used] = '\0';
    return used > 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    uint16_t target_port, origin_port;
    int listen_fd, enabled = 1, results = 0;
    if (argc != 5) return 2;
    target_port = parse_port(argv[1]);
    origin_port = parse_port(argv[2]);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(origin_port);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 8) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    if (send_search(target_port, origin_port, argv[3], argv[4]) != 0 ||
        send_search(target_port, origin_port, argv[3], argv[4]) != 0) {
        close(listen_fd);
        return 1;
    }
    while (1) {
        fd_set readable;
        struct timeval timeout = {1, 0};
        int ready;
        FD_ZERO(&readable);
        FD_SET(listen_fd, &readable);
        ready = select(listen_fd + 1, &readable, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            close(listen_fd);
            return 1;
        }
        if (ready == 0) break;
        if (FD_ISSET(listen_fd, &readable)) {
            char line[1024];
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) {
                close(listen_fd);
                return 1;
            }
            if (read_line(fd, line, sizeof(line)) == 0) {
                fputs(line, stdout);
                results++;
            }
            close(fd);
        }
    }
    printf("results %d\n", results);
    close(listen_fd);
    return results == 1 ? 0 : 1;
}
