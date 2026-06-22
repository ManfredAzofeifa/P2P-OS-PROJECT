/*
 * client.c - Cliente P2P principal.
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

// Convierte un texto que representa un numero de puerto en un valor uint16_t.
// Recibe: el texto con el numero a convertir, y donde guardar el resultado.
// Devuelve: 0 si el texto es un puerto valido (1-65535), -1 si es invalido o NULL.
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

// Elimina los caracteres de salto de linea ('\n', '\r') al final de una cadena.
// Recibe: la linea recibida por socket que puede traer saltos de linea del protocolo.
// Devuelve: nada; modifica la cadena en el lugar.
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

// Lee una linea completa desde un socket, caracter por caracter, hasta '\n' o fin de conexion.
// Recibe: el socket del que leer, el buffer donde guardar la linea, y el tamano maximo del buffer.
// Devuelve: 1 si se leyo al menos un caracter, 0 si la conexion cerro sin datos, -1 si hubo error.
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

// Envia un texto completo por socket, reintentando si el sistema operativo manda menos bytes de los pedidos.
// Recibe: el socket de destino, y el texto a enviar (terminado en '\0').
// Devuelve: 0 si se envio todo, -1 si hubo error de red.
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

// Construye una ruta de archivo uniendo la carpeta compartida con el nombre del archivo.
// Recibe: donde guardar la ruta armada, el tamano de ese buffer, la carpeta base, y el nombre del archivo.
// Devuelve: 0 si la ruta cabia en el buffer, -1 si la ruta resultante es demasiado larga.
static int armar_ruta(char *out, size_t out_size, const char *carpeta, const char *nombre) {
    int written;

    written = snprintf(out, out_size, "%s/%s", carpeta, nombre);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

// Escanea la carpeta compartida y carga en el contexto los metadatos (nombre, tamano, hash) de cada archivo regular.
// Recibe: el contexto del cliente, donde se guardaran los archivos encontrados y la ruta de la carpeta.
// Devuelve: 0 siempre; los errores individuales se reportan como advertencias pero no detienen el escaneo.
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

// Abre una conexion TCP hacia el servidor central del sistema P2P.
// Recibe: la direccion IP o nombre de host del servidor, y el puerto donde escucha.
// Devuelve: el file descriptor del socket conectado, o -1 si no se pudo conectar.
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

// Lee la lista de vecinos que el servidor envia tras el registro, y la guarda en el contexto.
// Recibe: el socket ya conectado al servidor con la respuesta NEIGHBORS lista para leer,
//         y el contexto donde guardar los vecinos recibidos.
// Devuelve: 0 si se recibio la lista completa hasta "END", -1 si la conexion fallo o el formato era invalido.
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

// Conecta al servidor y envia el mensaje REGISTER con todos los archivos locales para publicarlos en la red.
// Recibe: el contexto del cliente con la IP del servidor, los puertos, y la lista de archivos a registrar.
// Devuelve: 0 si el servidor respondio OK y se recibieron los vecinos, -1 si hubo algun fallo.
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

// Envia un FIND al servidor para buscar que pares tienen un archivo con ese nombre.
// Recibe: el contexto con la direccion del servidor, el nombre del archivo buscado,
//         el arreglo donde guardar los pares encontrados, cuantos caben, y donde escribir cuantos se recibieron.
// Devuelve: 0 si la busqueda termino correctamente (aunque no haya resultados), -1 si hubo error de red o protocolo.
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

// Envia un LOOKUP al servidor para buscar que pares tienen un archivo que coincida con el tamano y hash dados.
// Recibe: el contexto con la direccion del servidor, el tamano exacto del archivo buscado, su hash,
//         el arreglo donde guardar los pares, cuantos caben, y donde escribir cuantos se encontraron.
// Devuelve: 0 si la busqueda termino correctamente, -1 si hubo error de red o el protocolo respondio mal.
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

// Punto de entrada del cliente P2P: valida argumentos, carga archivos locales, se registra con el servidor,
// e inicia el servidor de transferencia y la consola interactiva.
// Recibe: los argumentos de linea de comandos: IP del servidor, puerto del servidor,
//         puerto de transferencia, y ruta de la carpeta compartida.
// Devuelve: 0 si termino correctamente, 1 si hubo error en ejecucion, 2 si los argumentos son invalidos.
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