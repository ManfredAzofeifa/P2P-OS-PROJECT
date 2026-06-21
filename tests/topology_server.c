#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_line(int fd, char *line, size_t size) {
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t n = recv(fd, line + used, 1, 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (line[used++] == '\n') break;
    }
    line[used] = '\0';
    return used > 0 ? 0 : -1;
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

static uint16_t port(const char *text) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end || !value || value > 65535UL) exit(2);
    return (uint16_t)value;
}

static void respond(int fd, uint16_t client, const uint16_t p[6],
                    unsigned int files) {
    uint16_t neighbors[2];
    size_t count = 0;
    char line[128];

    if (client == p[0]) neighbors[count++] = p[1];
    else if (client == p[1]) neighbors[count++] = p[2];
    else if (client == p[2]) {
        neighbors[count++] = p[0];
        neighbors[count++] = p[3];
    } else if (client == p[3]) neighbors[count++] = p[4];
    else if (client == p[4]) neighbors[count++] = p[5];

    snprintf(line, sizeof(line), "OK registered %u files\n", files);
    if (write_all(fd, line) != 0) return;
    snprintf(line, sizeof(line), "NEIGHBORS %zu\n", count);
    if (write_all(fd, line) != 0) return;
    for (size_t i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "NEIGHBOR 127.0.0.1 %u\n", neighbors[i]);
        if (write_all(fd, line) != 0) return;
    }
    write_all(fd, "END\n");
}

static void handle(int fd, const uint16_t ports[6]) {
    char line[1024];
    unsigned int client, files;
    if (read_line(fd, line, sizeof(line)) != 0 ||
        sscanf(line, "REGISTER %u %u", &client, &files) != 2 ||
        !client || client > 65535U) {
        write_all(fd, "ERROR invalid registration\n");
        return;
    }
    do {
        if (read_line(fd, line, sizeof(line)) != 0) return;
    } while (strcmp(line, "END\n") != 0);
    respond(fd, (uint16_t)client, ports, files);
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    uint16_t ports[6], listen_port;
    int listen_fd, enabled = 1;
    if (argc != 8) return 2;
    listen_port = port(argv[1]);
    for (size_t i = 0; i < 6; i++) ports[i] = port(argv[i + 2]);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(listen_port);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 16) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    printf("topology server listening on port %u\n", listen_port);
    fflush(stdout);
    while (1) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle(fd, ports);
        close(fd);
    }
    close(listen_fd);
    return 0;
}
