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

static void limpiar_salto_linea(char *linea) {
    size_t len = strlen(linea);

    while (len > 0 && (linea[len - 1] == '\n' || linea[len - 1] == '\r')) {
        linea[len - 1] = '\0';
        len--;
    }
}

static int leer_linea(int fd, char *buffer, size_t tamano) {
    size_t used = 0;

    while (used + 1 < tamano) {
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
    limpiar_salto_linea(buffer);
    return used > 0 ? 1 : 0;
}

static int escribir_todo(int fd, const void *datos, size_t len) {
    const char *bytes = datos;
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

static int escribir_texto(int fd, const char *texto) {
    return escribir_todo(fd, texto, strlen(texto));
}

static int serve_range(int fd, const char *ruta) {
    char linea[P2P_MAX_LINE];
    char header[P2P_MAX_LINE];
    char hash[P2P_HASH_STR_LEN];
    unsigned long long tamano;
    unsigned long long inicio;
    unsigned long long largo;
    unsigned char buffer[RANGE_BUFFER_SIZE];
    struct stat st;
    uint64_t restante;
    FILE *input;

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        return -1;
    }
    if (sscanf(linea, "GET %llu %32s %llu %llu", &tamano, hash, &inicio, &largo) != 4 ||
        strlen(hash) != P2P_HASH_HEX_LEN) {
        return escribir_texto(fd, "ERROR GET invalido\n");
    }
    if (stat(ruta, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != (uint64_t)tamano ||
        (uint64_t)inicio > (uint64_t)tamano || (uint64_t)largo > (uint64_t)tamano - (uint64_t)inicio) {
        return escribir_texto(fd, "ERROR rango invalido\n");
    }

    input = fopen(ruta, "rb");
    if (input == NULL) {
        return escribir_texto(fd, "ERROR archivo no disponible\n");
    }
    if (fseeko(input, (off_t)inicio, SEEK_SET) != 0) {
        fclose(input);
        return escribir_texto(fd, "ERROR fallo al moverse en archivo\n");
    }

    snprintf(header, sizeof(header), "DATA %llu\n", largo);
    if (escribir_texto(fd, header) != 0) {
        fclose(input);
        return -1;
    }

    restante = (uint64_t)largo;
    while (restante > 0) {
        size_t chunk = restante > sizeof(buffer) ? sizeof(buffer) : (size_t)restante;
        size_t nread = fread(buffer, 1, chunk, input);

        if (nread == 0) {
            fclose(input);
            return -1;
        }
        if (escribir_todo(fd, buffer, nread) != 0) {
            fclose(input);
            return -1;
        }
        restante -= nread;
    }

    fclose(input);
    return 0;
}

int main(int argc, char **argv) {
    unsigned long puerto;
    unsigned long max_requests;
    char *end = NULL;
    int listen_fd;
    int opt = 1;
    struct sockaddr_in addr;

    if (argc != 4) {
        fprintf(stderr, "uso: %s <puerto> <archivo> <max_requests>\n", argv[0]);
        return 2;
    }

    puerto = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || puerto == 0 || puerto > 65535UL) {
        fprintf(stderr, "puerto invalido\n");
        return 2;
    }
    end = NULL;
    max_requests = strtoul(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || max_requests == 0) {
        fprintf(stderr, "max_requests invalido\n");
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
        listen(listen_fd, 8) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("par de rangos escuchando en puerto %lu\n", puerto);
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
