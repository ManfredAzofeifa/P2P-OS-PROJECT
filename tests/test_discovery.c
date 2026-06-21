#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../distributed/discovery.h"

static int create_capture_listener(uint16_t *puerto) {
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int fd;
    int flags;
    int enabled = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    *puerto = ntohs(address.sin_port);
    return fd;
}

static int read_forwarded_line(int listen_fd, char *linea, size_t line_size) {
    int fd = accept(listen_fd, NULL, NULL);
    size_t used = 0;

    if (fd < 0) {
        return -1;
    }
    while (used + 1 < line_size) {
        ssize_t recibido = recv(fd, linea + used, 1, 0);

        if (recibido <= 0) {
            break;
        }
        if (linea[used++] == '\n') {
            break;
        }
    }
    linea[used] = '\0';
    close(fd);
    return used > 0 ? 0 : -1;
}

static int expect_no_forward(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);

    if (fd >= 0) {
        close(fd);
        return -1;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "prueba de busqueda distribuida fallo: %s\n", message);
        exit(1);
    }
}

int main(void) {
    contexto_cliente_p2p_t contexto;
    uint16_t capture_port;
    char linea[P2P_MAX_LINE];
    int listen_fd;
    time_t first_seen = 1000;

    memset(&contexto, 0, sizeof(contexto));
    listen_fd = create_capture_listener(&capture_port);
    require(listen_fd >= 0, "cannot create capture listener");
    snprintf(contexto.vecinos[0].ip, sizeof(contexto.vecinos[0].ip),
             "127.0.0.1");
    contexto.vecinos[0].puerto = capture_port;
    contexto.cantidad_vecinos = 1;

    require(manejar_mensaje_par_distribuido_en_momento(
                &contexto,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen) == 1,
            "TTL search was not handled");
    require(read_forwarded_line(listen_fd, linea, sizeof(linea)) == 0,
            "TTL search was not forwarded");
    require(strcmp(linea,
                   "DSEARCH 900 127.0.0.1 49999 2 needle\n") == 0,
            "forwarded TTL was not decremented");

    require(manejar_mensaje_par_distribuido_en_momento(
                &contexto,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen + 1) == 1,
            "repetido search was not handled");
    require(expect_no_forward(listen_fd) == 0,
            "repetido query was forwarded");

    require(manejar_mensaje_par_distribuido_en_momento(
                &contexto,
                "DSEARCH 901 127.0.0.1 49999 1 needle",
                first_seen + 2) == 1,
            "TTL-expired search was not handled");
    require(expect_no_forward(listen_fd) == 0,
            "TTL-expired search was forwarded");

    require(manejar_mensaje_par_distribuido_en_momento(
                &contexto,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen + P2P_SEEN_QUERY_TTL_SECONDS) == 1,
            "expired seen query was not handled");
    require(read_forwarded_line(listen_fd, linea, sizeof(linea)) == 0,
            "expired seen query was not accepted again");

    close(listen_fd);
    printf("busqueda distribuida ok\n");
    return 0;
}
