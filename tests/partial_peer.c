#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int escribir_todo(int fd, const char *datos, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t written = send(fd, datos + total, len - total, 0);
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
    unsigned long puerto;
    char *end = NULL;
    int listen_fd;
    int client_fd;
    int opt = 1;
    struct sockaddr_in addr;
    char buffer[256];

    if (argc != 2) {
        fprintf(stderr, "uso: %s <puerto>\n", argv[0]);
        return 2;
    }

    puerto = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || puerto == 0 || puerto > 65535UL) {
        fprintf(stderr, "puerto invalido\n");
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
    addr.sin_port = htons((uint16_t)puerto);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("par parcial escuchando en puerto %lu\n", puerto);
    fflush(stdout);

    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    recv(client_fd, buffer, sizeof(buffer), 0);
    escribir_todo(client_fd, "DATA 12\nabc", 11);
    close(client_fd);
    close(listen_fd);
    return 0;
}
