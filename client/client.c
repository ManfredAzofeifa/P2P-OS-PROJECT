/*
 * cliente.c - Cliente P2P principal.
 *
 * Documentacion de funciones:
 * leer_puerto: convierte texto a puerto; recibe texto y salida; valida argumentos CLI.
 * limpiar_salto_linea: limpia mensajes; recibe linea; ayuda al protocolo por sockets.
 * leer_linea: lee linea de socket; recibe fd, buffer y tamano; recibe respuestas del servidor.
 * escribir_todo: envia texto completo; recibe fd y texto; envia REGISTER, FIND y LOOKUP.
 * armar_ruta: une carpeta y nombre; recibe salida, tamano, carpeta y nombre; ubica archivos compartidos.
 * revisar_carpeta_compartida: escanea archivos y hashes; recibe contexto; registra archivos al iniciar.
 * conectar_servidor: abre TCP al servidor; recibe host y puerto; comunica cliente-servidor.
 * leer_vecinos: lee vecinos; recibe fd y contexto; guarda vecinos para busqueda distribuida.
 * registrar_con_servidor: envia metadatos; recibe contexto; publica archivos en servidor.
 * buscar_en_servidor: ejecuta FIND; recibe contexto, nombre y salida de pares; resuelve find -s.
 * buscar_hash_en_servidor: ejecuta LOOKUP; recibe contexto, tamano, hash y salida; apoya request.
 * main: inicia cliente; recibe servidor, puertos y carpeta; ejecuta registro, transferencia y consola.
 */

#include "client.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../server/hash.h"

static int leer_puerto(const char *texto, uint16_t *out) {
    unsigned long value;
    char *end = NULL;

    if (texto == NULL || out == NULL) {
        return -1;
    }

    value = strtoul(texto, &end, 10);
    if (*texto == '\0' || *end != '\0' || value == 0 || value > 65535UL) {
        return -1;
    }

    *out = (uint16_t)value;
    return 0;
}

static void limpiar_salto_linea(char *linea) {
    size_t len;

    if (linea == NULL) {
        return;
    }

    len = strlen(linea);
    while (len > 0 && (linea[len - 1] == '\n' || linea[len - 1] == '\r')) {
        linea[len - 1] = '\0';
        len--;
    }
}

static int leer_linea(int fd, char *buffer, size_t tamano) {
    size_t used = 0;

    if (buffer == NULL || tamano == 0) {
        return -1;
    }

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

static int escribir_todo(int fd, const char *texto) {
    size_t total = 0;
    size_t len = strlen(texto);

    while (total < len) {
        ssize_t nwritten = send(fd, texto + total, len - total, 0);

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

static int armar_ruta(char *out, size_t out_size, const char *carpeta, const char *nombre) {
    int written;

    written = snprintf(out, out_size, "%s/%s", carpeta, nombre);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

static int revisar_carpeta_compartida(contexto_cliente_p2p_t *contexto) {
    DIR *carpeta;
    struct dirent *entrada;

    contexto->cantidad_archivos = 0;
    carpeta = opendir(contexto->carpeta_compartida);
    if (carpeta == NULL) {
        fprintf(stderr, "warning: no se pudo abrir la carpeta compartida '%s': %s\n",
                contexto->carpeta_compartida, strerror(errno));
        return 0;
    }

    while ((entrada = readdir(carpeta)) != NULL) {
        char ruta[P2P_MAX_PATH];
        struct stat st;
        metadato_archivo_p2p_t *archivo;

        if (strcmp(entrada->d_name, ".") == 0 || strcmp(entrada->d_name, "..") == 0) {
            continue;
        }

        if (contexto->cantidad_archivos >= CLIENT_MAX_LOCAL_FILES) {
            fprintf(stderr, "warning: local archivo limit reached; skipping restante archivos\n");
            break;
        }

        if (strchr(entrada->d_name, '\n') != NULL || strlen(entrada->d_name) >= P2P_MAX_NAME) {
            fprintf(stderr, "warning: skipping unsupported archivo nombre '%s'\n", entrada->d_name);
            continue;
        }

        if (armar_ruta(ruta, sizeof(ruta), contexto->carpeta_compartida, entrada->d_name) != 0) {
            fprintf(stderr, "warning: skipping ruta that is too long: %s\n", entrada->d_name);
            continue;
        }

        if (stat(ruta, &st) != 0) {
            fprintf(stderr, "warning: cannot stat '%s': %s\n", ruta, strerror(errno));
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        archivo = &contexto->archivos[contexto->cantidad_archivos];
        memset(archivo, 0, sizeof(*archivo));
        snprintf(archivo->nombre, sizeof(archivo->nombre), "%s", entrada->d_name);
        archivo->tamano = (uint64_t)st.st_size;

        if (calcular_hash_archivo(ruta, archivo->hash) != 0) {
            fprintf(stderr, "warning: cannot hash '%s': %s\n", ruta, strerror(errno));
            continue;
        }

        contexto->cantidad_archivos++;
    }

    if (closedir(carpeta) != 0) {
        fprintf(stderr, "warning: cannot close shared folder '%s': %s\n",
                contexto->carpeta_compartida, strerror(errno));
    }

    return 0;
}

static int conectar_servidor(const char *host, uint16_t puerto) {
    struct addrinfo hints;
    struct addrinfo *resultados = NULL;
    struct addrinfo *it;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", puerto);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_text, &hints, &resultados) != 0) {
        return -1;
    }

    for (it = resultados; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(resultados);
    return fd;
}

static int leer_vecinos(int fd, contexto_cliente_p2p_t *contexto) {
    char linea[P2P_MAX_LINE];
    unsigned int esperados;

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        fprintf(stderr, "servidor closed before NEIGHBORS respuesta\n");
        return -1;
    }

    if (sscanf(linea, "NEIGHBORS %u", &esperados) != 1) {
        fprintf(stderr, "unexpected servidor respuesta: %s\n", linea);
        return -1;
    }

    contexto->cantidad_vecinos = 0;
    while (leer_linea(fd, linea, sizeof(linea)) > 0) {
        char ip[46];
        unsigned int puerto;

        if (strcmp(linea, "END") == 0) {
            return 0;
        }

        if (sscanf(linea, "NEIGHBOR %45s %u", ip, &puerto) == 2 &&
            puerto > 0 && puerto <= 65535U &&
            contexto->cantidad_vecinos < P2P_DEFAULT_NEIGHBORS) {
            punto_red_p2p_t *neighbor = &contexto->vecinos[contexto->cantidad_vecinos++];
            snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
            neighbor->puerto = (uint16_t)puerto;
        }
    }

    return -1;
}

static int registrar_con_servidor(contexto_cliente_p2p_t *contexto) {
    int fd;
    char linea[P2P_MAX_LINE];
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = sizeof(local_addr);

    fd = conectar_servidor(contexto->ip_servidor, contexto->puerto_servidor);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to servidor %s:%u\n",
                contexto->ip_servidor, contexto->puerto_servidor);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
        void *address = NULL;
        if (local_addr.ss_family == AF_INET) {
            address = &((struct sockaddr_in *)&local_addr)->sin_addr;
        } else if (local_addr.ss_family == AF_INET6) {
            address = &((struct sockaddr_in6 *)&local_addr)->sin6_addr;
        }
        if (address != NULL) {
            inet_ntop(local_addr.ss_family, address,
                      contexto->ip_local, sizeof(contexto->ip_local));
        }
    }


    snprintf(linea, sizeof(linea), "REGISTER %u %zu\n",
             contexto->puerto_transferencia, contexto->cantidad_archivos);
    if (escribir_todo(fd, linea) != 0) {
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < contexto->cantidad_archivos; i++) {
        snprintf(linea, sizeof(linea), "FILE %llu %s %s\n",
                 (unsigned long long)contexto->archivos[i].tamano,
                 contexto->archivos[i].hash,
                 contexto->archivos[i].nombre);
        if (escribir_todo(fd, linea) != 0) {
            close(fd);
            return -1;
        }
    }

    if (escribir_todo(fd, "END\n") != 0) {
        close(fd);
        return -1;
    }

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        fprintf(stderr, "servidor closed before registration respuesta\n");
        close(fd);
        return -1;
    }

    if (strncmp(linea, "OK ", 3) != 0) {
        fprintf(stderr, "registro fallo: %s\n", linea);
        close(fd);
        return -1;
    }

    if (leer_vecinos(fd, contexto) != 0) {
        close(fd);
        return -1;
    }

    close(fd);
    printf("registrados %zu archivos con %s:%u\n",
           contexto->cantidad_archivos, contexto->ip_servidor, contexto->puerto_servidor);
    printf("recibidos %zu vecinos\n", contexto->cantidad_vecinos);
    return 0;
}

int buscar_en_servidor(const contexto_cliente_p2p_t *contexto,
                          const char *nombre,
                          punto_red_p2p_t *pares,
                          size_t max_pares,
                          size_t *cantidad_pares) {
    int fd;
    char linea[P2P_MAX_LINE];
    unsigned int esperados;

    if (contexto == NULL || nombre == NULL || pares == NULL || cantidad_pares == NULL ||
        nombre[0] == '\0' || strchr(nombre, '\n') != NULL) {
        return -1;
    }

    *cantidad_pares = 0;
    fd = conectar_servidor(contexto->ip_servidor, contexto->puerto_servidor);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to servidor %s:%u\n",
                contexto->ip_servidor, contexto->puerto_servidor);
        return -1;
    }

    snprintf(linea, sizeof(linea), "FIND %s\n", nombre);
    if (escribir_todo(fd, linea) != 0) {
        close(fd);
        return -1;
    }

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        fprintf(stderr, "servidor closed before FIND respuesta\n");
        close(fd);
        return -1;
    }

    if (sscanf(linea, "PEERS %u", &esperados) != 1) {
        fprintf(stderr, "unexpected FIND respuesta: %s\n", linea);
        close(fd);
        return -1;
    }

    while (leer_linea(fd, linea, sizeof(linea)) > 0) {
        char ip[46];
        unsigned int puerto;

        if (strcmp(linea, "END") == 0) {
            close(fd);
            return 0;
        }

        if (sscanf(linea, "PEER %45s %u", ip, &puerto) == 2 &&
            puerto > 0 && puerto <= 65535U &&
            *cantidad_pares < max_pares) {
            punto_red_p2p_t *peer = &pares[*cantidad_pares];
            snprintf(peer->ip, sizeof(peer->ip), "%s", ip);
            peer->puerto = (uint16_t)puerto;
            (*cantidad_pares)++;
        }
    }

    close(fd);
    return -1;
}

int buscar_hash_en_servidor(const contexto_cliente_p2p_t *contexto,
                            uint64_t tamano,
                            const char *hash,
                            punto_red_p2p_t *pares,
                            size_t max_pares,
                            size_t *cantidad_pares) {
    int fd;
    char linea[P2P_MAX_LINE];
    unsigned int esperados;

    if (contexto == NULL || hash == NULL || pares == NULL || cantidad_pares == NULL ||
        strlen(hash) != P2P_HASH_HEX_LEN || strchr(hash, '\n') != NULL) {
        return -1;
    }

    *cantidad_pares = 0;
    fd = conectar_servidor(contexto->ip_servidor, contexto->puerto_servidor);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to servidor %s:%u\n",
                contexto->ip_servidor, contexto->puerto_servidor);
        return -1;
    }

    snprintf(linea, sizeof(linea), "LOOKUP %llu %s\n", (unsigned long long)tamano, hash);
    if (escribir_todo(fd, linea) != 0) {
        close(fd);
        return -1;
    }

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        fprintf(stderr, "servidor closed before LOOKUP respuesta\n");
        close(fd);
        return -1;
    }

    if (sscanf(linea, "PEERS %u", &esperados) != 1) {
        fprintf(stderr, "unexpected LOOKUP respuesta: %s\n", linea);
        close(fd);
        return -1;
    }

    while (leer_linea(fd, linea, sizeof(linea)) > 0) {
        char ip[46];
        unsigned int puerto;

        if (strcmp(linea, "END") == 0) {
            close(fd);
            return 0;
        }

        if (sscanf(linea, "PEER %45s %u", ip, &puerto) == 2 &&
            puerto > 0 && puerto <= 65535U &&
            *cantidad_pares < max_pares) {
            punto_red_p2p_t *peer = &pares[*cantidad_pares];
            snprintf(peer->ip, sizeof(peer->ip), "%s", ip);
            peer->puerto = (uint16_t)puerto;
            (*cantidad_pares)++;
        }
    }

    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    contexto_cliente_p2p_t contexto;

    if (argc != 5) {
        fprintf(stderr, "uso: %s <ip_servidor> <puerto_servidor> <puerto_transferencia> <carpeta_compartida>\n",
                argv[0]);
        return 2;
    }

    memset(&contexto, 0, sizeof(contexto));
    snprintf(contexto.ip_servidor, sizeof(contexto.ip_servidor), "%s", argv[1]);
    snprintf(contexto.carpeta_compartida, sizeof(contexto.carpeta_compartida), "%s", argv[4]);

    if (leer_puerto(argv[2], &contexto.puerto_servidor) != 0) {
        fprintf(stderr, "puerto del servidor invalido: %s\n", argv[2]);
        return 2;
    }

    if (leer_puerto(argv[3], &contexto.puerto_transferencia) != 0) {
        fprintf(stderr, "puerto de transferencia invalido: %s\n", argv[3]);
        return 2;
    }

    revisar_carpeta_compartida(&contexto);

    if (registrar_con_servidor(&contexto) != 0) {
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (iniciar_servidor_transferencia(&contexto) != 0) {
        fprintf(stderr, "cannot start transfer servidor on puerto %u\n", contexto.puerto_transferencia);
        return 1;
    }

    correr_consola_cliente(&contexto);
    return 0;
}
