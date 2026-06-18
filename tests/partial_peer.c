#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t written = send(fd, data + total, len - total, 0);
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

int main(int argc, char **argv) {
    unsigned long port;
    char *end = NULL;
    int listen_fd;
    int client_fd;
    int opt = 1;
    struct sockaddr_in addr;
    char buffer[256];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }

    port = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port == 0 || port > 65535UL) {
        fprintf(stderr, "invalid port\n");
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
        listen(listen_fd, 1) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("partial peer listening on port %lu\n", port);
    fflush(stdout);

    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    recv(client_fd, buffer, sizeof(buffer), 0);
    write_all(client_fd, "DATA 12\nabc", 11);
    close(client_fd);
    close(listen_fd);
    return 0;
}
