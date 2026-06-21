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

static uint16_t leer_puerto(const char *texto) {
    char *end = NULL;
    unsigned long value = strtoul(texto, &end, 10);
    if (!texto[0] || *end || !value || value > 65535UL) exit(2);
    return (uint16_t)value;
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

static int send_search(uint16_t target_port, uint16_t origin_port,
                       const char *id_consulta, const char *termino) {
    struct sockaddr_in address;
    char linea[1024];
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
    snprintf(linea, sizeof(linea), "DSEARCH %s 127.0.0.1 %u 4 %s\n",
             id_consulta, origin_port, termino);
    if (escribir_todo(fd, linea) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int leer_linea(int fd, char *linea, size_t tamano) {
    size_t used = 0;
    while (used + 1 < tamano) {
        ssize_t n = recv(fd, linea + used, 1, 0);
        if (n <= 0) break;
        if (linea[used++] == '\n') break;
    }
    linea[used] = '\0';
    return used > 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    uint16_t target_port, origin_port;
    int listen_fd, enabled = 1, resultados = 0;
    if (argc != 5) return 2;
    target_port = leer_puerto(argv[1]);
    origin_port = leer_puerto(argv[2]);

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
            char linea[1024];
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) {
                close(listen_fd);
                return 1;
            }
            if (leer_linea(fd, linea, sizeof(linea)) == 0) {
                fputs(linea, stdout);
                resultados++;
            }
            close(fd);
        }
    }
    printf("resultados %d\n", resultados);
    close(listen_fd);
    return resultados == 1 ? 0 : 1;
}
