#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int leer_linea(int fd, char *linea, size_t tamano) {
    size_t used = 0;
    while (used + 1 < tamano) {
        ssize_t n = recv(fd, linea + used, 1, 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (linea[used++] == '\n') break;
    }
    linea[used] = '\0';
    return used > 0 ? 0 : -1;
}

static int escribir_todo(int fd, const char *texto) {
    size_t total = 0, largo = strlen(texto);
    while (total < largo) {
        ssize_t n = send(fd, texto + total, largo - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static uint16_t puerto(const char *texto) {
    char *end = NULL;
    unsigned long value = strtoul(texto, &end, 10);
    if (!texto[0] || *end || !value || value > 65535UL) exit(2);
    return (uint16_t)value;
}

static void respond(int fd, uint16_t cliente, const uint16_t p[6],
                    unsigned int archivos) {
    uint16_t vecinos[2];
    size_t count = 0;
    char linea[128];

    if (cliente == p[0]) vecinos[count++] = p[1];
    else if (cliente == p[1]) vecinos[count++] = p[2];
    else if (cliente == p[2]) {
        vecinos[count++] = p[0];
        vecinos[count++] = p[3];
    } else if (cliente == p[3]) vecinos[count++] = p[4];
    else if (cliente == p[4]) vecinos[count++] = p[5];

    snprintf(linea, sizeof(linea), "OK registrado %u archivos\n", archivos);
    if (escribir_todo(fd, linea) != 0) return;
    snprintf(linea, sizeof(linea), "NEIGHBORS %zu\n", count);
    if (escribir_todo(fd, linea) != 0) return;
    for (size_t i = 0; i < count; i++) {
        snprintf(linea, sizeof(linea), "NEIGHBOR 127.0.0.1 %u\n", vecinos[i]);
        if (escribir_todo(fd, linea) != 0) return;
    }
    escribir_todo(fd, "END\n");
}

static void handle(int fd, const uint16_t ports[6]) {
    char linea[1024];
    unsigned int cliente, archivos;
    if (leer_linea(fd, linea, sizeof(linea)) != 0 ||
        sscanf(linea, "REGISTER %u %u", &cliente, &archivos) != 2 ||
        !cliente || cliente > 65535U) {
        escribir_todo(fd, "ERROR registro invalido\n");
        return;
    }
    do {
        if (leer_linea(fd, linea, sizeof(linea)) != 0) return;
    } while (strcmp(linea, "END\n") != 0);
    respond(fd, (uint16_t)cliente, ports, archivos);
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    uint16_t ports[6], listen_port;
    int listen_fd, enabled = 1;
    if (argc != 8) return 2;
    listen_port = puerto(argv[1]);
    for (size_t i = 0; i < 6; i++) ports[i] = puerto(argv[i + 2]);

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
    printf("servidor de topologia escuchando en puerto %u\n", listen_port);
    fflush(stdout);
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
