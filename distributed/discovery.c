/*
 * discovery.c - Protocolo de busqueda distribuida.
 *
 * Documentacion de funciones:
 * escribir_todo: envia texto por socket; recibe fd y texto; manda DSEARCH/DRESULT completos.
 * conectar_punto: conecta con un vecino; recibe host y puerto; permite inundar busquedas.
 * entrada_vista_vencida: revisa expiracion; recibe entrada y tiempo actual; limpia IDs viejos.
 * recordar_consulta: guarda ID visto; recibe id y tiempo; evita reenviar duplicados.
 * atender_resultado: guarda DRESULT recibido; recibe linea; entrega resultados al originador.
 * manejar_mensaje_par_distribuido: procesa DSEARCH/DRESULT; recibe contexto y linea; resuelve mensajes entre clientes.
 * manejar_mensaje_par_distribuido_en_momento: version con tiempo fijo; recibe contexto, linea y tiempo; permite probar expiracion.
 * buscar_en_vecinos: inicia busqueda distribuida; recibe contexto, termino y salidas; resuelve find -d.
 */

#include "discovery.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DISTRIBUTED_RESULT_WAIT_MILLISECONDS 500
#define DISTRIBUTED_MAX_SEEN_QUERIES 1024

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t resultado_listo;
    unsigned long long id_consulta_activa;
    metadato_archivo_p2p_t *resultados;
    size_t max_resultados;
    size_t cantidad_resultados;
    int activo;
} estado_consulta_distribuida_t;
typedef struct {
    unsigned long long id_consulta;
    time_t visto_en;
} entrada_consulta_vista_t;

typedef struct {
    pthread_mutex_t mutex;
    entrada_consulta_vista_t entradas[DISTRIBUTED_MAX_SEEN_QUERIES];
    size_t count;
} estado_consultas_vistas_t;


static estado_consulta_distribuida_t estado_consulta = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    NULL,
    0,
    0,
    0
};
static unsigned long long next_query_id = 1;
static estado_consultas_vistas_t consultas_vistas = {
    PTHREAD_MUTEX_INITIALIZER, {{0, 0}}, 0
};


static int escribir_todo(int fd, const char *texto) {
    size_t total = 0;
    size_t largo = strlen(texto);

    while (total < largo) {
        ssize_t written = send(fd, texto + total, largo - total, 0);

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

static int conectar_punto(const char *host, uint16_t puerto) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", puerto);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_text, &hints, &addresses) != 0) {
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int entrada_vista_vencida(const entrada_consulta_vista_t *entrada, time_t ahora) {
    return ahora >= entrada->visto_en &&
           ahora - entrada->visto_en >= P2P_SEEN_QUERY_TTL_SECONDS;
}

static int recordar_consulta(unsigned long long id_consulta, time_t ahora) {
    size_t oldest = 0;

    pthread_mutex_lock(&consultas_vistas.mutex);
    for (size_t i = 0; i < consultas_vistas.count;) {
        if (entrada_vista_vencida(&consultas_vistas.entradas[i], ahora)) {
            consultas_vistas.entradas[i] = consultas_vistas.entradas[--consultas_vistas.count];
            continue;
        }
        if (consultas_vistas.entradas[i].id_consulta == id_consulta) {
            pthread_mutex_unlock(&consultas_vistas.mutex);
            return 0;
        }
        if (consultas_vistas.entradas[i].visto_en <
            consultas_vistas.entradas[oldest].visto_en) {
            oldest = i;
        }
        i++;
    }

    if (consultas_vistas.count < DISTRIBUTED_MAX_SEEN_QUERIES) {
        oldest = consultas_vistas.count++;
    }
    consultas_vistas.entradas[oldest].id_consulta = id_consulta;
    consultas_vistas.entradas[oldest].visto_en = ahora;
    pthread_mutex_unlock(&consultas_vistas.mutex);
    return 1;
}

static void reenviar_busqueda(const contexto_cliente_p2p_t *contexto,
                           unsigned long long id_consulta,
                           const char *origin_ip,
                           uint16_t origin_port,
                           unsigned int ttl,
                           const char *termino) {
    for (size_t i = 0; i < contexto->cantidad_vecinos; i++) {
        char linea[P2P_MAX_LINE];
        int fd = conectar_punto(contexto->vecinos[i].ip,
                                     contexto->vecinos[i].puerto);

        if (fd < 0) {
            continue;
        }
        snprintf(linea, sizeof(linea), "DSEARCH %llu %s %u %u %s\n",
                 id_consulta, origin_ip, origin_port, ttl, termino);
        escribir_todo(fd, linea);
        close(fd);
    }
}

static void enviar_resultado(const contexto_cliente_p2p_t *contexto,
                        unsigned long long id_consulta,
                        const char *origin_ip,
                        uint16_t origin_port,
                        const metadato_archivo_p2p_t *archivo) {
    char linea[P2P_MAX_LINE];
    int fd = conectar_punto(origin_ip, origin_port);

    if (fd < 0) {
        return;
    }
    snprintf(linea, sizeof(linea), "DRESULT %llu %llu %s %s %s %u\n",
             id_consulta, (unsigned long long)archivo->tamano, archivo->hash, archivo->nombre,
             contexto->ip_local, contexto->puerto_transferencia);
    escribir_todo(fd, linea);
    close(fd);
}

static int atender_busqueda_distribuida(const contexto_cliente_p2p_t *contexto,
                         const char *linea,
                         time_t ahora) {
    unsigned long long id_consulta;
    unsigned int origin_port;
    unsigned int ttl;
    char origin_ip[46];
    char termino[P2P_MAX_NAME];

    if (sscanf(linea, "DSEARCH %llu %45s %u %u %255s",
               &id_consulta, origin_ip, &origin_port, &ttl, termino) != 5 ||
        origin_port == 0 || origin_port > 65535U || ttl == 0) {
        return 1;
    }

    if (!recordar_consulta(id_consulta, ahora)) {
        return 1;
    }

    for (size_t i = 0; i < contexto->cantidad_archivos; i++) {
        if (strcmp(contexto->archivos[i].nombre, termino) == 0) {
            enviar_resultado(contexto, id_consulta, origin_ip, (uint16_t)origin_port,
                        &contexto->archivos[i]);
        }
    }
    if (ttl > 1) {
        reenviar_busqueda(contexto, id_consulta, origin_ip, (uint16_t)origin_port,
                       ttl - 1, termino);
    }

    return 1;
}

static int atender_resultado(const char *linea) {
    unsigned long long id_consulta;
    unsigned long long tamano;
    unsigned int owner_port;
    char hash[P2P_HASH_STR_LEN];
    char nombre[P2P_MAX_NAME];
    char owner_ip[46];

    if (sscanf(linea, "DRESULT %llu %llu %32s %255s %45s %u",
               &id_consulta, &tamano, hash, nombre, owner_ip, &owner_port) != 6 ||
        strlen(hash) != P2P_HASH_HEX_LEN ||
        owner_port == 0 || owner_port > 65535U) {
        return 1;
    }

    pthread_mutex_lock(&estado_consulta.mutex);
    if (estado_consulta.activo &&
        estado_consulta.id_consulta_activa == id_consulta &&
        estado_consulta.cantidad_resultados < estado_consulta.max_resultados) {
        metadato_archivo_p2p_t *result;

        result = &estado_consulta.resultados[estado_consulta.cantidad_resultados++];
        memset(result, 0, sizeof(*result));
        result->tamano = (uint64_t)tamano;
        snprintf(result->hash, sizeof(result->hash), "%s", hash);
        snprintf(result->nombre, sizeof(result->nombre), "%s", nombre);
        snprintf(result->dueno.ip, sizeof(result->dueno.ip), "%s", owner_ip);
        result->dueno.puerto = (uint16_t)owner_port;
        pthread_cond_signal(&estado_consulta.resultado_listo);
    }
    pthread_mutex_unlock(&estado_consulta.mutex);
    return 1;
}

int manejar_mensaje_par_distribuido_en_momento(const contexto_cliente_p2p_t *contexto,
                                       const char *linea,
                                       time_t ahora) {
    if (contexto == NULL || linea == NULL) {
        return 0;
    }
    if (strncmp(linea, "DSEARCH ", 8) == 0) {
        return atender_busqueda_distribuida(contexto, linea, ahora);
    }
    if (strncmp(linea, "DRESULT ", 8) == 0) {
        return atender_resultado(linea);
    }
    return 0;
}

int manejar_mensaje_par_distribuido(const contexto_cliente_p2p_t *contexto,
                                    const char *linea) {
    return manejar_mensaje_par_distribuido_en_momento(contexto, linea, time(NULL));
}

int buscar_en_vecinos(const contexto_cliente_p2p_t *contexto,
                                 const char *termino,
                                 metadato_archivo_p2p_t *resultados,
                                 size_t max_resultados,
                                 size_t *cantidad_resultados) {
    struct timespec deadline;
    unsigned long long id_consulta;
    size_t sent = 0;

    if (contexto == NULL || termino == NULL || resultados == NULL ||
        cantidad_resultados == NULL || termino[0] == '\0' ||
        strlen(termino) >= P2P_MAX_NAME || strchr(termino, '\n') != NULL) {
        return -1;
    }
    *cantidad_resultados = 0;

    pthread_mutex_lock(&estado_consulta.mutex);
    id_consulta = ((unsigned long long)time(NULL) << 32) ^
               ((unsigned long long)getpid() << 16) ^
               contexto->puerto_transferencia ^ next_query_id++;
    estado_consulta.id_consulta_activa = id_consulta;
    estado_consulta.resultados = resultados;
    estado_consulta.max_resultados = max_resultados;
    estado_consulta.cantidad_resultados = 0;
    estado_consulta.activo = 1;
    pthread_mutex_unlock(&estado_consulta.mutex);

    recordar_consulta(id_consulta, time(NULL));
    for (size_t i = 0; i < contexto->cantidad_vecinos; i++) {
        char linea[P2P_MAX_LINE];
        int fd = conectar_punto(contexto->vecinos[i].ip,
                                     contexto->vecinos[i].puerto);

        if (fd < 0) {
            continue;
        }
        snprintf(linea, sizeof(linea), "DSEARCH %llu %s %u %u %s\n",
                 id_consulta, contexto->ip_local, contexto->puerto_transferencia,
                 P2P_DEFAULT_TTL, termino);
        if (escribir_todo(fd, linea) == 0) {
            sent++;
        }
        close(fd);
    }

    if (sent > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += DISTRIBUTED_RESULT_WAIT_MILLISECONDS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&estado_consulta.mutex);
        while (pthread_cond_timedwait(&estado_consulta.resultado_listo,
                                      &estado_consulta.mutex, &deadline) == 0) {
        }
        pthread_mutex_unlock(&estado_consulta.mutex);
    }

    pthread_mutex_lock(&estado_consulta.mutex);
    *cantidad_resultados = estado_consulta.cantidad_resultados;
    estado_consulta.activo = 0;
    estado_consulta.resultados = NULL;
    pthread_mutex_unlock(&estado_consulta.mutex);
    return 0;
}
