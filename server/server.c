/*
 * servidor.c - Servidor P2P principal.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../protocol/protocol.h"
#include "metadata.h"

#define SERVER_BACKLOG 32
#define MAX_REGISTERED_FILES 4096
#define MAX_REGISTERED_CLIENTS 256

typedef struct {
    int fd;
    struct sockaddr_storage addr;
} client_connection_t;

static metadato_archivo_p2p_t archivos_registrados[MAX_REGISTERED_FILES];
static size_t cantidad_archivos_registrados = 0;
static punto_red_p2p_t clientes_recientes[MAX_REGISTERED_CLIENTS];
static size_t cantidad_clientes_recientes = 0;
static pthread_mutex_t candado_registro = PTHREAD_MUTEX_INITIALIZER;

// Elimina los caracteres de salto de linea ('\n', '\r') al final de una cadena.
// Recibe: la linea recibida por socket que puede traer saltos de linea del protocolo textual.
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
// Recibe: el socket del cliente, el buffer donde guardar la linea, y el tamano maximo del buffer.
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
// Recibe: el socket del cliente de destino, y el texto de respuesta a enviar.
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

// Extrae la direccion IP y asigna el puerto de transferencia desde la direccion de socket de un cliente.
// Recibe: la direccion de socket del cliente recien conectado, el puerto de transferencia que el cliente declaro,
//         una IP IPv4 anunciada opcionalmente, y el endpoint donde guardar la IP y el puerto.
// Devuelve: 0 si pudo construir un endpoint valido, -1 si la IP anunciada era invalida.
static int punto_desde_sockaddr(const struct sockaddr_storage *addr,
                                uint16_t puerto_transferencia,
                                const char *ip_anunciada,
                                punto_red_p2p_t *endpoint) {
    const void *src = NULL;
    struct in_addr ipv4;

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->puerto = puerto_transferencia;

    if (ip_anunciada != NULL && ip_anunciada[0] != '\0') {
        if (inet_pton(AF_INET, ip_anunciada, &ipv4) != 1) {
            return -1;
        }
        snprintf(endpoint->ip, sizeof(endpoint->ip), "%s", ip_anunciada);
        return 0;
    }

    if (addr->ss_family == AF_INET) {
        src = &((const struct sockaddr_in *)addr)->sin_addr;
    } else if (addr->ss_family == AF_INET6) {
        src = &((const struct sockaddr_in6 *)addr)->sin6_addr;
    }

    if (src == NULL ||
        inet_ntop(addr->ss_family, src, endpoint->ip, sizeof(endpoint->ip)) == NULL) {
        return -1;
    }
    return 0;
}

// Compara si dos endpoints (IP + puerto) representan el mismo nodo de la red.
// Recibe: los dos puntos de red a comparar.
// Devuelve: 1 si tienen la misma IP y el mismo puerto, 0 si difieren en alguno.
static int mismo_punto(const punto_red_p2p_t *a, const punto_red_p2p_t *b) {
    return a->puerto == b->puerto && strcmp(a->ip, b->ip) == 0;
}

// Elimina del registro global todos los archivos que pertenecen a un cliente dado.
// Recibe: el endpoint del dueno cuyos archivos deben borrarse del indice del servidor.
// Devuelve: nada; modifica el arreglo global de archivos registrados en el lugar.
static void quitar_archivos_del_dueno(const punto_red_p2p_t *dueno) {
    size_t i = 0;

    while (i < cantidad_archivos_registrados) {
        if (mismo_punto(&archivos_registrados[i].dueno, dueno)) {
            archivos_registrados[i] = archivos_registrados[cantidad_archivos_registrados - 1];
            cantidad_archivos_registrados--;
        } else {
            i++;
        }
    }
}

// Actualiza la lista de clientes recientes, moviendo al cliente al final si ya existia o agregandolo nuevo.
// Recibe: el endpoint del cliente que acaba de registrarse exitosamente.
// Devuelve: nada; modifica el arreglo global de clientes recientes.
static void guardar_cliente_reciente(const punto_red_p2p_t *endpoint) {
    size_t i;

    for (i = 0; i < cantidad_clientes_recientes; i++) {
        if (mismo_punto(&clientes_recientes[i], endpoint)) {
            size_t j;
            punto_red_p2p_t existente = clientes_recientes[i];

            for (j = i; j + 1 < cantidad_clientes_recientes; j++) {
                clientes_recientes[j] = clientes_recientes[j + 1];
            }
            clientes_recientes[cantidad_clientes_recientes - 1] = existente;
            return;
        }
    }

    if (cantidad_clientes_recientes < MAX_REGISTERED_CLIENTS) {
        clientes_recientes[cantidad_clientes_recientes++] = *endpoint;
        return;
    }

    memmove(&clientes_recientes[0], &clientes_recientes[1], sizeof(clientes_recientes[0]) * (MAX_REGISTERED_CLIENTS - 1));
    clientes_recientes[MAX_REGISTERED_CLIENTS - 1] = *endpoint;
}

// Selecciona los clientes mas recientes para devolvercelos como vecinos al cliente que se esta registrando.
// Recibe: el endpoint del cliente que se esta registrando (para excluirse a si mismo),
//         el arreglo donde guardar los vecinos seleccionados, y cuantos vecinos como maximo devolver.
// Devuelve: la cantidad de vecinos efectivamente seleccionados.
static size_t juntar_vecinos(const punto_red_p2p_t *self,
                                punto_red_p2p_t *vecinos,
                                size_t max_vecinos) {
    size_t count = 0;
    size_t restante = cantidad_clientes_recientes;

    while (restante > 0 && count < max_vecinos) {
        const punto_red_p2p_t *candidato = &clientes_recientes[restante - 1];
        if (!mismo_punto(candidato, self)) {
            vecinos[count++] = *candidato;
        }
        restante--;
    }

    return count;
}

// Procesa una sesion de registro: lee los archivos enviados por el cliente, los indexa, y responde con vecinos.
// Recibe: el socket del cliente, la direccion de red del cliente,
//         y la primera linea del mensaje REGISTER ya leida con el puerto y cantidad de archivos.
// Devuelve: nada; envia la respuesta OK y la lista de vecinos, o un ERROR si el protocolo falla.
static void atender_registro(int fd,
                            const struct sockaddr_storage *addr,
                            const char *linea) {
    unsigned int puerto_transferencia;
    unsigned int archivos_esperados;
    char ip_anunciada[46] = "";
    int campos_registro;
    punto_red_p2p_t dueno;
    metadato_archivo_p2p_t pendientes[MAX_REGISTERED_FILES];
    size_t cantidad_pendientes = 0;
    punto_red_p2p_t vecinos[P2P_DEFAULT_NEIGHBORS];
    size_t cantidad_vecinos;
    char respuesta[P2P_MAX_LINE];

    campos_registro = sscanf(linea, "REGISTER %u %u %45s",
                             &puerto_transferencia, &archivos_esperados, ip_anunciada);
    if ((campos_registro != 2 && campos_registro != 3) ||
        puerto_transferencia == 0 ||
        puerto_transferencia > 65535U) {
        escribir_todo(fd, "ERROR REGISTER invalido\n");
        return;
    }

    if (punto_desde_sockaddr(addr, (uint16_t)puerto_transferencia,
                             campos_registro == 3 ? ip_anunciada : NULL, &dueno) != 0) {
        escribir_todo(fd, "ERROR IP anunciada invalida\n");
        return;
    }

    while (cantidad_pendientes < MAX_REGISTERED_FILES) {
        char file_line[P2P_MAX_LINE];
        int estado = leer_linea(fd, file_line, sizeof(file_line));
        unsigned long long tamano;
        char hash[P2P_HASH_STR_LEN];
        char nombre[P2P_MAX_NAME];

        if (estado <= 0) {
            escribir_todo(fd, "ERROR REGISTER incompleto\n");
            return;
        }
        if (strcmp(file_line, "END") == 0) {
            break;
        }

        if (sscanf(file_line, "FILE %llu %32s %255[^\n]", &tamano, hash, nombre) != 3) {
            escribir_todo(fd, "ERROR FILE invalido\n");
            return;
        }

        memset(&pendientes[cantidad_pendientes], 0, sizeof(pendientes[cantidad_pendientes]));
        snprintf(pendientes[cantidad_pendientes].nombre, sizeof(pendientes[cantidad_pendientes].nombre), "%s", nombre);
        snprintf(pendientes[cantidad_pendientes].hash, sizeof(pendientes[cantidad_pendientes].hash), "%s", hash);
        pendientes[cantidad_pendientes].tamano = (uint64_t)tamano;
        pendientes[cantidad_pendientes].dueno = dueno;
        cantidad_pendientes++;
    }

    pthread_mutex_lock(&candado_registro);
    cantidad_vecinos = juntar_vecinos(&dueno, vecinos, P2P_DEFAULT_NEIGHBORS);
    quitar_archivos_del_dueno(&dueno);
    guardar_cliente_reciente(&dueno);

    for (size_t i = 0; i < cantidad_pendientes && cantidad_archivos_registrados < MAX_REGISTERED_FILES; i++) {
        archivos_registrados[cantidad_archivos_registrados++] = pendientes[i];
    }
    pthread_mutex_unlock(&candado_registro);

    (void)archivos_esperados;

    snprintf(respuesta, sizeof(respuesta), "OK registrado %zu archivos\n", cantidad_pendientes);
    escribir_todo(fd, respuesta);
    snprintf(respuesta, sizeof(respuesta), "NEIGHBORS %zu\n", cantidad_vecinos);
    escribir_todo(fd, respuesta);
    for (size_t i = 0; i < cantidad_vecinos; i++) {
        snprintf(respuesta, sizeof(respuesta), "NEIGHBOR %s %u\n",
                 vecinos[i].ip, vecinos[i].puerto);
        escribir_todo(fd, respuesta);
    }
    escribir_todo(fd, "END\n");
}

// Procesa un FIND: busca en el indice global que clientes tienen un archivo con ese nombre y devuelve sus endpoints.
// Recibe: el socket del cliente, y la linea del mensaje FIND con el nombre del archivo a buscar.
// Devuelve: nada; envia PEERS con la lista de pares encontrados, o ERROR si el mensaje es invalido.
static void atender_find(int fd, const char *linea) {
    char nombre[P2P_MAX_NAME];
    punto_red_p2p_t pares[P2P_MAX_PEERS];
    size_t cantidad_pares = 0;
    char respuesta[P2P_MAX_LINE];

    if (sscanf(linea, "FIND %255[^\n]", nombre) != 1) {
        escribir_todo(fd, "ERROR FIND invalido\n");
        return;
    }

    pthread_mutex_lock(&candado_registro);
    for (size_t i = 0; i < cantidad_archivos_registrados && cantidad_pares < P2P_MAX_PEERS; i++) {
        if (strcmp(archivos_registrados[i].nombre, nombre) == 0) {
            int repetido = 0;
            for (size_t j = 0; j < cantidad_pares; j++) {
                if (mismo_punto(&pares[j], &archivos_registrados[i].dueno)) {
                    repetido = 1;
                    break;
                }
            }
            if (!repetido) {
                pares[cantidad_pares++] = archivos_registrados[i].dueno;
            }
        }
    }
    pthread_mutex_unlock(&candado_registro);

    snprintf(respuesta, sizeof(respuesta), "PEERS %zu\n", cantidad_pares);
    escribir_todo(fd, respuesta);
    for (size_t i = 0; i < cantidad_pares; i++) {
        snprintf(respuesta, sizeof(respuesta), "PEER %s %u\n", pares[i].ip, pares[i].puerto);
        escribir_todo(fd, respuesta);
    }
    escribir_todo(fd, "END\n");
}

// Procesa un LOOKUP: busca en el indice global que clientes tienen un archivo con ese tamano y hash exactos.
// Recibe: el socket del cliente, y la linea del mensaje LOOKUP con el tamano y el hash a buscar.
// Devuelve: nada; envia PEERS con los pares encontrados, o ERROR si el mensaje es invalido.
static void atender_lookup(int fd, const char *linea) {
    unsigned long long tamano;
    char hash[P2P_HASH_STR_LEN];
    punto_red_p2p_t pares[P2P_MAX_PEERS];
    size_t cantidad_pares = 0;
    char respuesta[P2P_MAX_LINE];

    if (sscanf(linea, "LOOKUP %llu %32s", &tamano, hash) != 2) {
        escribir_todo(fd, "ERROR LOOKUP invalido\n");
        return;
    }

    pthread_mutex_lock(&candado_registro);
    for (size_t i = 0; i < cantidad_archivos_registrados && cantidad_pares < P2P_MAX_PEERS; i++) {
        if (archivos_registrados[i].tamano == (uint64_t)tamano && strcmp(archivos_registrados[i].hash, hash) == 0) {
            int repetido = 0;
            for (size_t j = 0; j < cantidad_pares; j++) {
                if (mismo_punto(&pares[j], &archivos_registrados[i].dueno)) {
                    repetido = 1;
                    break;
                }
            }
            if (!repetido) {
                pares[cantidad_pares++] = archivos_registrados[i].dueno;
            }
        }
    }
    pthread_mutex_unlock(&candado_registro);

    snprintf(respuesta, sizeof(respuesta), "PEERS %zu\n", cantidad_pares);
    escribir_todo(fd, respuesta);
    for (size_t i = 0; i < cantidad_pares; i++) {
        snprintf(respuesta, sizeof(respuesta), "PEER %s %u\n", pares[i].ip, pares[i].puerto);
        escribir_todo(fd, respuesta);
    }
    escribir_todo(fd, "END\n");
}

// Atiende una conexion de cliente en su propio hilo: lee el primer comando y delega al handler correspondiente.
// Recibe: el socket y la direccion del cliente aceptados por el listener principal, empaquetados en un client_connection_t.
// Devuelve: NULL siempre; libera la memoria del argumento antes de terminar.
static void *hilo_cliente(void *arg) {
    client_connection_t *connection = arg;
    char linea[P2P_MAX_LINE];

    if (leer_linea(connection->fd, linea, sizeof(linea)) > 0) {
        if (strncmp(linea, "REGISTER ", 9) == 0) {
            atender_registro(connection->fd, &connection->addr, linea);
        } else if (strncmp(linea, "FIND ", 5) == 0) {
            atender_find(connection->fd, linea);
        } else if (strncmp(linea, "LOOKUP ", 7) == 0) {
            atender_lookup(connection->fd, linea);
        } else {
            escribir_todo(connection->fd, "ERROR comando desconocido\n");
        }
    }

    close(connection->fd);
    free(connection);
    return NULL;
}

// Crea y configura un socket TCP en el puerto indicado, listo para aceptar conexiones entrantes.
// Recibe: el numero de puerto en el que el servidor debe escuchar.
// Devuelve: el file descriptor del socket en escucha, o -1 si no se pudo crear, enlazar, o poner en escucha.
static int crear_socket_escucha(uint16_t puerto) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(puerto);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SERVER_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Punto de entrada del servidor P2P: valida el puerto, abre el socket de escucha, y atiende conexiones en bucle.
// Recibe: el numero de puerto como argumento de linea de comandos.
// Devuelve: 2 si el argumento es invalido, 1 si fallo la inicializacion del socket, nunca retorna 0 en operacion normal.
int main(int argc, char **argv) {
    unsigned long port_value;
    char *end = NULL;
    int listen_fd;

    if (argc != 2) {
        fprintf(stderr, "uso: %s <puerto>\n", argv[0]);
        return 2;
    }

    port_value = strtoul(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port_value == 0 || port_value > 65535UL) {
        fprintf(stderr, "puerto invalido: %s\n", argv[1]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);

    listen_fd = crear_socket_escucha((uint16_t)port_value);
    if (listen_fd < 0) {
        perror("listen socket");
        return 1;
    }

    printf("P2P servidor escuchando en puerto %lu\n", port_value);
    fflush(stdout);

    while (1) {
        client_connection_t *connection;
        socklen_t addr_len;
        pthread_t hilo;

        connection = calloc(1, sizeof(*connection));
        if (connection == NULL) {
            continue;
        }

        addr_len = sizeof(connection->addr);
        connection->fd = accept(listen_fd, (struct sockaddr *)&connection->addr, &addr_len);
        if (connection->fd < 0) {
            free(connection);
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        if (pthread_create(&hilo, NULL, hilo_cliente, connection) != 0) {
            close(connection->fd);
            free(connection);
            continue;
        }
        pthread_detach(hilo);
    }

    close(listen_fd);
    return 1;
}
