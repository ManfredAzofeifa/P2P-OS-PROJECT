/*
 * transfer.c - Envio y recepcion de archivos por segmentos.
 */

#include "transfer.h"
#include "../distributed/discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../server/hash.h"

#define TRANSFER_BACKLOG 16
#define TRANSFER_BUFFER_SIZE 8192

typedef struct {
    int fd;
    const contexto_cliente_p2p_t *contexto;
} pedido_transferencia_t;

typedef struct {
    int listen_fd;
    const contexto_cliente_p2p_t *contexto;
} servidor_transferencia_t;

// Elimina los caracteres de salto de linea ('\n', '\r') al final de una cadena.
// Recibe: la linea recibida por socket que puede traer saltos de linea del protocolo de transferencia.
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
// Recibe: el socket del par conectado, el buffer donde guardar la linea, y el tamano maximo del buffer.
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

// Envia un bloque de bytes completo por socket, reintentando si el sistema operativo manda menos de los pedidos.
// Recibe: el socket de destino, el bloque de datos a enviar (puede ser binario), y la cantidad de bytes a enviar.
// Devuelve: 0 si se enviaron todos los bytes, -1 si hubo error de red.
static int escribir_todo(int fd, const void *datos, size_t len) {
    const char *bytes = datos;
    size_t total = 0;

    while (total < len) {
        ssize_t nwritten = send(fd, bytes + total, len - total, 0);

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

// Envia una cadena de texto completa por socket usando escribir_todo.
// Recibe: el socket de destino, y el texto a enviar (encabezados del protocolo o mensajes de error).
// Devuelve: 0 si se envio todo, -1 si hubo error de red.
static int escribir_texto(int fd, const char *texto) {
    return escribir_todo(fd, texto, strlen(texto));
}

// Construye una ruta de archivo uniendo la carpeta compartida con el nombre del archivo.
// Recibe: donde guardar la ruta armada, el tamano de ese buffer, la carpeta base, y el nombre del archivo.
// Devuelve: 0 si la ruta cabia en el buffer, -1 si la ruta resultante es demasiado larga.
static int armar_ruta(char *out, size_t out_size, const char *carpeta, const char *nombre) {
    int written = snprintf(out, out_size, "%s/%s", carpeta, nombre);

    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

// Busca en los archivos locales del cliente cual coincide con el tamano y el hash pedidos.
// Recibe: el contexto del cliente con la lista de archivos locales, el tamano exacto buscado, y el hash buscado.
// Devuelve: puntero al metadato del archivo encontrado, o NULL si ningun archivo local coincide.
static const metadato_archivo_p2p_t *buscar_archivo_local(const contexto_cliente_p2p_t *contexto,
                                                  uint64_t tamano,
                                                  const char *hash) {
    for (size_t i = 0; i < contexto->cantidad_archivos; i++) {
        const metadato_archivo_p2p_t *archivo = &contexto->archivos[i];
        if (archivo->tamano == tamano && strcmp(archivo->hash, hash) == 0) {
            return archivo;
        }
    }

    return NULL;
}

// Lee un rango del archivo local indicado y lo envia al socket solicitante con el encabezado DATA.
// Recibe: el socket del par que solicita el rango,
//         el contexto del cliente con la carpeta compartida,
//         el metadato del archivo a leer con su nombre y tamano esperado,
//         el byte de inicio del rango dentro del archivo,
//         y la cantidad de bytes a enviar desde ese inicio.
// Devuelve: 0 si el rango se envio completo, -1 si el archivo no esta disponible o hubo error al leerlo o enviarlo.
static int enviar_rango_archivo(int fd,
                           const contexto_cliente_p2p_t *contexto,
                           const metadato_archivo_p2p_t *archivo,
                           uint64_t inicio,
                           uint64_t largo) {
    char ruta[P2P_MAX_PATH];
    char header[P2P_MAX_LINE];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    FILE *input;
    struct stat st;
    uint64_t restante = largo;

    if (inicio > archivo->tamano || largo > archivo->tamano - inicio) {
        return escribir_texto(fd, "ERROR rango invalido\n");
    }

    if (armar_ruta(ruta, sizeof(ruta), contexto->carpeta_compartida, archivo->nombre) != 0) {
        return escribir_texto(fd, "ERROR ruta demasiado larga\n");
    }

    if (stat(ruta, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != archivo->tamano) {
        return escribir_texto(fd, "ERROR archivo no disponible\n");
    }

    input = fopen(ruta, "rb");
    if (input == NULL) {
        return escribir_texto(fd, "ERROR archivo no disponible\n");
    }

    if (fseeko(input, (off_t)inicio, SEEK_SET) != 0) {
        fclose(input);
        return escribir_texto(fd, "ERROR fallo al moverse en archivo\n");
    }

    snprintf(header, sizeof(header), "DATA %llu\n", (unsigned long long)largo);
    if (escribir_texto(fd, header) != 0) {
        fclose(input);
        return -1;
    }

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

// Atiende en su propio hilo una solicitud GET entrante de otro cliente: busca el archivo localmente y envia el rango pedido.
// Recibe: un pedido_transferencia_t con el socket del par solicitante y el contexto del cliente actual.
// Devuelve: NULL siempre; libera la memoria del argumento antes de terminar.
static void *hilo_pedido_transferencia(void *arg) {
    pedido_transferencia_t *pedido = arg;
    char linea[P2P_MAX_LINE];
    unsigned long long tamano;
    unsigned long long inicio;
    unsigned long long largo;
    char hash[P2P_HASH_STR_LEN];
    const metadato_archivo_p2p_t *archivo;

    if (leer_linea(pedido->fd, linea, sizeof(linea)) <= 0) {
        close(pedido->fd);
        free(pedido);
        return NULL;
    }
    if (manejar_mensaje_par_distribuido(pedido->contexto, linea)) {
        close(pedido->fd);
        free(pedido);
        return NULL;
    }


    if (sscanf(linea, "GET %llu %32s %llu %llu", &tamano, hash, &inicio, &largo) != 4 ||
        strlen(hash) != P2P_HASH_HEX_LEN) {
        escribir_texto(pedido->fd, "ERROR GET invalido\n");
        close(pedido->fd);
        free(pedido);
        return NULL;
    }

    archivo = buscar_archivo_local(pedido->contexto, (uint64_t)tamano, hash);
    if (archivo == NULL) {
        escribir_texto(pedido->fd, "ERROR archivo no encontrado\n");
        close(pedido->fd);
        free(pedido);
        return NULL;
    }

    enviar_rango_archivo(pedido->fd, pedido->contexto, archivo, (uint64_t)inicio, (uint64_t)largo);
    close(pedido->fd);
    free(pedido);
    return NULL;
}

// Corre en un hilo de fondo aceptando conexiones entrantes de pares y creando un hilo por cada solicitud GET.
// Recibe: un servidor_transferencia_t con el socket en escucha y el contexto del cliente.
// Devuelve: NULL siempre; libera la memoria del servidor antes de terminar.
static void *hilo_aceptar_transferencias(void *arg) {
    servidor_transferencia_t *servidor = arg;

    while (1) {
        pedido_transferencia_t *pedido;
        pthread_t hilo;
        int fd = accept(servidor->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        pedido = calloc(1, sizeof(*pedido));
        if (pedido == NULL) {
            close(fd);
            continue;
        }

        pedido->fd = fd;
        pedido->contexto = servidor->contexto;

        if (pthread_create(&hilo, NULL, hilo_pedido_transferencia, pedido) != 0) {
            close(fd);
            free(pedido);
            continue;
        }
        pthread_detach(hilo);
    }

    close(servidor->listen_fd);
    free(servidor);
    return NULL;
}

// Crea y configura un socket TCP en el puerto indicado para recibir solicitudes GET de otros clientes.
// Recibe: el puerto donde el cliente publicara su servidor de transferencia.
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

    if (listen(fd, TRANSFER_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Lee exactamente la cantidad de bytes indicada desde un socket, bloqueando hasta recibirlos todos.
// Recibe: el socket del par que esta enviando datos, el buffer donde guardarlos, y la cantidad exacta esperada.
// Devuelve: 0 si se recibieron todos los bytes, -1 si la conexion cerro antes o hubo error.
static int leer_exacto(int fd, unsigned char *buffer, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t nread = recv(fd, buffer + total, len - total, 0);

        if (nread == 0) {
            return -1;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)nread;
    }

    return 0;
}

// Abre una conexion TCP hacia un par de la red para descargarle un rango de archivo.
// Recibe: el endpoint del par con su IP y puerto de transferencia.
// Devuelve: el file descriptor del socket conectado, o -1 si la conexion fallo.
static int conectar_par(const punto_red_p2p_t *par) {
    struct addrinfo hints;
    struct addrinfo *resultados = NULL;
    struct addrinfo *it;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", par->puerto);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(par->ip, port_text, &hints, &resultados) != 0) {
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

// Verifica si un archivo en disco coincide exactamente con el tamano y hash esperados.
// Recibe: la ruta del archivo a verificar, el tamano esperado en bytes, y el hash esperado en hexadecimal.
// Devuelve: 1 si el archivo existe y coincide, 0 si no existe, -1 si existe pero no coincide o no se pudo verificar.
static int archivo_calza_con_pedido(const char *ruta, uint64_t tamano, const char *hash) {
    struct stat st;
    char actual_hash[P2P_HASH_STR_LEN];

    if (stat(ruta, &st) != 0) {
        return 0;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != tamano) {
        return -1;
    }
    if (calcular_hash_archivo(ruta, actual_hash) != 0) {
        return -1;
    }
    return strcmp(actual_hash, hash) == 0 ? 1 : -1;
}

// Construye la ruta del archivo temporal de descarga agregando la extension ".part" a la ruta final.
// Recibe: donde guardar la ruta parcial armada, el tamano de ese buffer, y la ruta definitiva del archivo.
// Devuelve: 0 si la ruta parcial cabia en el buffer, -1 si resultaba demasiado larga.
static int armar_ruta_parcial(char *out, size_t out_size, const char *ruta) {
    int written = snprintf(out, out_size, "%s.part", ruta);

    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

// Descarga el archivo completo desde un unico par usando el protocolo GET, verificando hash al final.
// Recibe: el contexto del cliente con la carpeta de destino,
//         el par desde el que descargar con su IP y puerto,
//         el tamano exacto del archivo a descargar,
//         el hash esperado del archivo,
//         el buffer donde escribir la ruta donde quedo guardado el archivo,
//         el tamano de ese buffer,
//         y donde indicar si el archivo ya existia previamente.
// Devuelve: 0 si la descarga fue exitosa o el archivo ya existia, -1 si hubo algun fallo.
int descargar_de_par(const contexto_cliente_p2p_t *contexto,
                                const punto_red_p2p_t *par,
                                uint64_t tamano,
                                const char *hash,
                                char *ruta_guardada,
                                size_t tamano_ruta_guardada,
                                int *ya_existe) {
    int fd;
    char pedido[P2P_MAX_LINE];
    char linea[P2P_MAX_LINE];
    char ruta[P2P_MAX_PATH];
    char partial_path[P2P_MAX_PATH];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    unsigned long long data_length;
    uint64_t restante = tamano;
    FILE *output;
    int existing_status;

    if (ya_existe != NULL) {
        *ya_existe = 0;
    }
    if (contexto == NULL || par == NULL || hash == NULL || strlen(hash) != P2P_HASH_HEX_LEN) {
        return -1;
    }
    if (armar_ruta(ruta, sizeof(ruta), contexto->carpeta_compartida, hash) != 0 ||
        armar_ruta_parcial(partial_path, sizeof(partial_path), ruta) != 0) {
        return -1;
    }

    existing_status = archivo_calza_con_pedido(ruta, tamano, hash);
    if (existing_status > 0) {
        if (ruta_guardada != NULL && tamano_ruta_guardada > 0) {
            snprintf(ruta_guardada, tamano_ruta_guardada, "%s", ruta);
        }
        if (ya_existe != NULL) {
            *ya_existe = 1;
        }
        return 0;
    }
    if (existing_status < 0) {
        return -1;
    }

    fd = conectar_par(par);
    if (fd < 0) {
        return -1;
    }

    snprintf(pedido, sizeof(pedido), "GET %llu %s 0 %llu\n",
             (unsigned long long)tamano, hash, (unsigned long long)tamano);
    if (escribir_texto(fd, pedido) != 0) {
        close(fd);
        return -1;
    }

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        close(fd);
        return -1;
    }

    if (strncmp(linea, "ERROR ", 6) == 0) {
        fprintf(stderr, "par %s:%u returned %s\n", par->ip, par->puerto, linea);
        close(fd);
        return -1;
    }
    if (sscanf(linea, "DATA %llu", &data_length) != 1 || data_length != tamano) {
        close(fd);
        return -1;
    }

    unlink(partial_path);
    output = fopen(partial_path, "wb");
    if (output == NULL) {
        close(fd);
        return -1;
    }

    while (restante > 0) {
        size_t chunk = restante > sizeof(buffer) ? sizeof(buffer) : (size_t)restante;

        if (leer_exacto(fd, buffer, chunk) != 0) {
            fclose(output);
            close(fd);
            unlink(partial_path);
            return -1;
        }
        if (fwrite(buffer, 1, chunk, output) != chunk) {
            fclose(output);
            close(fd);
            unlink(partial_path);
            return -1;
        }
        restante -= chunk;
    }

    if (fclose(output) != 0) {
        close(fd);
        unlink(partial_path);
        return -1;
    }
    close(fd);

    if (archivo_calza_con_pedido(partial_path, tamano, hash) <= 0) {
        unlink(partial_path);
        return -1;
    }
    if (link(partial_path, ruta) != 0) {
        unlink(partial_path);
        return -1;
    }
    unlink(partial_path);

    if (ruta_guardada != NULL && tamano_ruta_guardada > 0) {
        snprintf(ruta_guardada, tamano_ruta_guardada, "%s", ruta);
    }
    return 0;
}

typedef struct {
    const contexto_cliente_p2p_t *contexto;
    const punto_red_p2p_t *par;
    uint64_t tamano;
    const char *hash;
    const char *partial_path;
    uint64_t inicio;
    uint64_t largo;
    int result;
} range_download_t;

// Crea en disco un archivo vacio del tamano indicado, reservando el espacio para una descarga segmentada.
// Recibe: la ruta donde crear el archivo, y el tamano en bytes que debe tener.
// Devuelve: 0 si el archivo se creo y reservo correctamente, -1 si no se pudo crear o reservar espacio.
static int crear_archivo_vacio(const char *ruta, uint64_t tamano) {
    int fd = open(ruta, O_CREAT | O_TRUNC | O_WRONLY, 0600);

    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)tamano) != 0) {
        close(fd);
        unlink(ruta);
        return -1;
    }
    close(fd);
    return 0;
}

// Descarga un rango especifico de bytes desde un par y los escribe en la posicion correcta del archivo parcial.
// Recibe: el par desde el que descargar con su IP y puerto,
//         el tamano total del archivo (para el mensaje GET),
//         el hash del archivo (para el mensaje GET),
//         el byte de inicio del rango a descargar,
//         la cantidad de bytes del rango,
//         y la ruta del archivo parcial compartido donde escribir los bytes recibidos.
// Devuelve: 0 si el rango se descargo y escribio correctamente, -1 si hubo cualquier fallo.
static int download_range_to_path(const punto_red_p2p_t *par,
                                  uint64_t tamano,
                                  const char *hash,
                                  uint64_t inicio,
                                  uint64_t largo,
                                  const char *partial_path) {
    int fd;
    char pedido[P2P_MAX_LINE];
    char linea[P2P_MAX_LINE];
    unsigned char buffer[TRANSFER_BUFFER_SIZE];
    unsigned long long data_length;
    uint64_t restante = largo;
    FILE *output;

    fd = conectar_par(par);
    if (fd < 0) {
        return -1;
    }

    snprintf(pedido, sizeof(pedido), "GET %llu %s %llu %llu\n",
             (unsigned long long)tamano, hash,
             (unsigned long long)inicio, (unsigned long long)largo);
    if (escribir_texto(fd, pedido) != 0) {
        close(fd);
        return -1;
    }

    if (leer_linea(fd, linea, sizeof(linea)) <= 0) {
        close(fd);
        return -1;
    }
    if (strncmp(linea, "ERROR ", 6) == 0) {
        fprintf(stderr, "par %s:%u returned %s\n", par->ip, par->puerto, linea);
        close(fd);
        return -1;
    }
    if (sscanf(linea, "DATA %llu", &data_length) != 1 || data_length != largo) {
        close(fd);
        return -1;
    }

    output = fopen(partial_path, "r+b");
    if (output == NULL) {
        close(fd);
        return -1;
    }
    if (fseeko(output, (off_t)inicio, SEEK_SET) != 0) {
        fclose(output);
        close(fd);
        return -1;
    }

    while (restante > 0) {
        size_t chunk = restante > sizeof(buffer) ? sizeof(buffer) : (size_t)restante;

        if (leer_exacto(fd, buffer, chunk) != 0) {
            fclose(output);
            close(fd);
            return -1;
        }
        if (fwrite(buffer, 1, chunk, output) != chunk) {
            fclose(output);
            close(fd);
            return -1;
        }
        restante -= chunk;
    }

    if (fclose(output) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

// Ejecuta en su propio hilo la descarga de un rango del archivo desde el par asignado.
// Recibe: un range_download_t con el par, el rango a descargar, y la ruta del archivo parcial compartido.
// Devuelve: NULL siempre; escribe el resultado de la descarga en el campo result del argumento.
static void *hilo_descarga_rango(void *arg) {
    range_download_t *range = arg;

    range->result = download_range_to_path(range->par, range->tamano, range->hash,
                                           range->inicio, range->largo,
                                           range->partial_path);
    return NULL;
}

// Verifica que el archivo parcial sea correcto y lo renombra atomicamente a su ruta final.
// Recibe: la ruta del archivo parcial a validar e instalar,
//         la ruta definitiva donde debe quedar el archivo,
//         el tamano esperado, y el hash esperado para validarlo.
// Devuelve: 0 si el archivo fue validado e instalado correctamente, -1 si la validacion fallo o el link no se pudo crear.
static int install_partial_file(const char *partial_path,
                                const char *ruta,
                                uint64_t tamano,
                                const char *hash) {
    if (archivo_calza_con_pedido(partial_path, tamano, hash) <= 0) {
        unlink(partial_path);
        return -1;
    }
    if (link(partial_path, ruta) != 0) {
        unlink(partial_path);
        return -1;
    }
    unlink(partial_path);
    return 0;
}

// Descarga un archivo distribuyendo el trabajo entre multiples pares en paralelo, cada uno con un rango diferente.
// Recibe: el contexto del cliente con la carpeta de destino,
//         la lista de pares candidatos y cuantos hay,
//         el tamano exacto del archivo a descargar,
//         el hash esperado del archivo,
//         el buffer donde escribir la ruta donde quedo guardado el archivo y su tamano,
//         donde indicar si el archivo ya existia previamente,
//         y donde indicar si la descarga fue segmentada entre multiples pares.
// Devuelve: 0 si la descarga fue exitosa o el archivo ya existia, -1 si algun rango fallo y no pudo recuperarse.
int descargar_de_pares(const contexto_cliente_p2p_t *contexto,
                                 const punto_red_p2p_t *pares,
                                 size_t cantidad_pares,
                                 uint64_t tamano,
                                 const char *hash,
                                 char *ruta_guardada,
                                 size_t tamano_ruta_guardada,
                                 int *ya_existe,
                                 int *segmentado) {
    char ruta[P2P_MAX_PATH];
    char partial_path[P2P_MAX_PATH];
    range_download_t ranges[P2P_MAX_PEERS];
    pthread_t threads[P2P_MAX_PEERS];
    size_t range_count;
    uint64_t inicio = 0;
    uint64_t base;
    uint64_t extra;
    int existing_status;
    int failed = 0;

    if (ya_existe != NULL) {
        *ya_existe = 0;
    }
    if (segmentado != NULL) {
        *segmentado = 0;
    }
    if (contexto == NULL || pares == NULL || cantidad_pares == 0 ||
        hash == NULL || strlen(hash) != P2P_HASH_HEX_LEN) {
        return -1;
    }

    if (cantidad_pares == 1 || tamano == 0) {
        return descargar_de_par(contexto, &pares[0], tamano, hash,
                                           ruta_guardada, tamano_ruta_guardada,
                                           ya_existe);
    }

    if (armar_ruta(ruta, sizeof(ruta), contexto->carpeta_compartida, hash) != 0 ||
        armar_ruta_parcial(partial_path, sizeof(partial_path), ruta) != 0) {
        return -1;
    }

    existing_status = archivo_calza_con_pedido(ruta, tamano, hash);
    if (existing_status > 0) {
        if (ruta_guardada != NULL && tamano_ruta_guardada > 0) {
            snprintf(ruta_guardada, tamano_ruta_guardada, "%s", ruta);
        }
        if (ya_existe != NULL) {
            *ya_existe = 1;
        }
        return 0;
    }
    if (existing_status < 0) {
        return -1;
    }

    range_count = cantidad_pares > P2P_MAX_PEERS ? P2P_MAX_PEERS : cantidad_pares;
    if ((uint64_t)range_count > tamano) {
        range_count = (size_t)tamano;
    }

    unlink(partial_path);
    if (crear_archivo_vacio(partial_path, tamano) != 0) {
        return -1;
    }

    base = tamano / range_count;
    extra = tamano % range_count;
    memset(ranges, 0, sizeof(ranges));
    for (size_t i = 0; i < range_count; i++) {
        ranges[i].contexto = contexto;
        ranges[i].par = &pares[i];
        ranges[i].tamano = tamano;
        ranges[i].hash = hash;
        ranges[i].partial_path = partial_path;
        ranges[i].inicio = inicio;
        ranges[i].largo = base + (i < extra ? 1 : 0);
        ranges[i].result = -1;
        inicio += ranges[i].largo;

        if (pthread_create(&threads[i], NULL, hilo_descarga_rango, &ranges[i]) != 0) {
            ranges[i].result = -1;
            failed = 1;
            for (size_t j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            break;
        }
    }

    if (!failed) {
        for (size_t i = 0; i < range_count; i++) {
            pthread_join(threads[i], NULL);
            if (ranges[i].result != 0) {
                failed = 1;
            }
        }
    }

    if (failed) {
        for (size_t i = 0; i < range_count; i++) {
            if (ranges[i].result == 0) {
                continue;
            }

            ranges[i].result = -1;
            for (size_t j = 0; j < cantidad_pares && j < P2P_MAX_PEERS; j++) {
                if (&pares[j] == ranges[i].par) {
                    continue;
                }
                if (download_range_to_path(&pares[j], tamano, hash,
                                           ranges[i].inicio, ranges[i].largo,
                                           partial_path) == 0) {
                    ranges[i].result = 0;
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < range_count; i++) {
        if (ranges[i].result != 0) {
            unlink(partial_path);
            return -1;
        }
    }

    if (install_partial_file(partial_path, ruta, tamano, hash) != 0) {
        return -1;
    }

    if (ruta_guardada != NULL && tamano_ruta_guardada > 0) {
        snprintf(ruta_guardada, tamano_ruta_guardada, "%s", ruta);
    }
    if (segmentado != NULL) {
        *segmentado = 1;
    }
    return 0;
}

// Inicia el servidor de transferencia del cliente en un hilo de fondo, dejandolo listo para recibir GET de otros pares.
// Recibe: el contexto del cliente con el puerto de transferencia donde escuchar y los archivos disponibles para servir.
// Devuelve: 0 si el servidor arranco correctamente, -1 si no se pudo crear el socket o el hilo.
int iniciar_servidor_transferencia(const contexto_cliente_p2p_t *contexto) {
    servidor_transferencia_t *servidor;
    pthread_t hilo;
    int listen_fd;

    if (contexto == NULL) {
        return -1;
    }

    listen_fd = crear_socket_escucha(contexto->puerto_transferencia);
    if (listen_fd < 0) {
        return -1;
    }

    servidor = calloc(1, sizeof(*servidor));
    if (servidor == NULL) {
        close(listen_fd);
        return -1;
    }

    servidor->listen_fd = listen_fd;
    servidor->contexto = contexto;

    if (pthread_create(&hilo, NULL, hilo_aceptar_transferencias, servidor) != 0) {
        close(listen_fd);
        free(servidor);
        return -1;
    }

    pthread_detach(hilo);
    printf("servidor de transferencia escuchando en puerto %u\n", contexto->puerto_transferencia);
    return 0;
}