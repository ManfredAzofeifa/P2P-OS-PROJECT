/*
 * console.c - Interfaz de consola: find y request.
 */

#include "client.h"
#include "transfer.h"
#include "../distributed/discovery.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Imprime en pantalla la lista de archivos locales del cliente, con tamano, hash y nombre.
// Recibe: el contexto del cliente con el arreglo de archivos locales ya cargados.
// Devuelve: nada.
static void mostrar_archivos(const contexto_cliente_p2p_t *contexto) {
    if (contexto->cantidad_archivos == 0) {
        printf("no hay archivos locales\n");
        return;
    }

    for (size_t i = 0; i < contexto->cantidad_archivos; i++) {
        printf("%llu %s %s\n",
               (unsigned long long)contexto->archivos[i].tamano,
               contexto->archivos[i].hash,
               contexto->archivos[i].nombre);
    }
}

// Imprime en pantalla la lista de vecinos conocidos, con su IP y puerto de transferencia.
// Recibe: el contexto del cliente con la lista de vecinos recibida al registrarse.
// Devuelve: nada.
static void mostrar_vecinos(const contexto_cliente_p2p_t *contexto) {
    if (contexto->cantidad_vecinos == 0) {
        printf("no hay vecinos\n");
        return;
    }

    for (size_t i = 0; i < contexto->cantidad_vecinos; i++) {
        printf("%s %u\n", contexto->vecinos[i].ip, contexto->vecinos[i].puerto);
    }
}

// Ejecuta una busqueda por nombre en el servidor central e imprime los pares que tienen el archivo.
// Recibe: el contexto del cliente con la direccion del servidor, y el nombre del archivo a buscar.
// Devuelve: nada; los resultados se imprimen directamente en pantalla.
static void atender_find_servidor(const contexto_cliente_p2p_t *contexto, const char *nombre) {
    punto_red_p2p_t pares[P2P_MAX_PEERS];
    size_t cantidad_pares = 0;

    if (nombre == NULL || nombre[0] == '\0') {
        printf("uso: find -s <nombre>\n");
        return;
    }

    if (buscar_en_servidor(contexto, nombre, pares, P2P_MAX_PEERS, &cantidad_pares) != 0) {
        printf("fallo la busqueda en servidor\n");
        return;
    }

    if (cantidad_pares == 0) {
        printf("no se encontraron pares para %s\n", nombre);
        return;
    }

    printf("pares para %s: %zu\n", nombre, cantidad_pares);
    for (size_t i = 0; i < cantidad_pares; i++) {
        printf("%s %u\n", pares[i].ip, pares[i].puerto);
    }
}

// Ejecuta una busqueda distribuida por nombre entre los vecinos e imprime los resultados con su dueno.
// Recibe: el contexto del cliente con la lista de vecinos, y el nombre del archivo a buscar.
// Devuelve: nada; los resultados se imprimen directamente en pantalla.
static void handle_find_distributed(const contexto_cliente_p2p_t *contexto,
                                    const char *nombre) {
    metadato_archivo_p2p_t resultados[P2P_MAX_PEERS];
    size_t cantidad_resultados = 0;

    if (nombre == NULL || nombre[0] == '\0') {
        printf("uso: find -d <nombre>\n");
        return;
    }
    if (buscar_en_vecinos(contexto, nombre, resultados,
                                     P2P_MAX_PEERS, &cantidad_resultados) != 0) {
        printf("fallo la busqueda distribuida\n");
        return;
    }
    if (cantidad_resultados == 0) {
        printf("no hay resultados distribuidos para %s\n", nombre);
        return;
    }
    printf("resultados distribuidos para %s: %zu\n", nombre, cantidad_resultados);
    for (size_t i = 0; i < cantidad_resultados; i++) {
        printf("%llu %s %s %s %u\n",
               (unsigned long long)resultados[i].tamano, resultados[i].hash,
               resultados[i].nombre, resultados[i].dueno.ip, resultados[i].dueno.puerto);
    }
}

// Extrae el tamano y el hash desde el texto que viene despues del comando "request".
// Recibe: el texto con los argumentos en formato "<tamano> <hash>",
//         donde guardar el tamano parseado, y donde guardar el hash.
// Devuelve: 0 si los argumentos son validos, -1 si faltan datos, el hash es incorrecto, o el tamano no es numerico.
static int leer_argumentos_request(const char *args, uint64_t *tamano, char hash[P2P_HASH_STR_LEN]) {
    char size_text[32];
    char hash_text[P2P_HASH_STR_LEN];
    unsigned long long parsed_size;
    char *end = NULL;

    if (args == NULL || tamano == NULL || hash == NULL) {
        return -1;
    }

    if (sscanf(args, "%31s %32s", size_text, hash_text) != 2) {
        return -1;
    }

    errno = 0;
    parsed_size = strtoull(size_text, &end, 10);
    if (errno != 0 || *size_text == '\0' || *end != '\0') {
        return -1;
    }

    if (strlen(hash_text) != P2P_HASH_HEX_LEN) {
        return -1;
    }

    *tamano = (uint64_t)parsed_size;
    snprintf(hash, P2P_HASH_STR_LEN, "%s", hash_text);
    return 0;
}

// Procesa el comando "request": busca pares en el servidor por tamano/hash y descarga el archivo.
// Recibe: el contexto del cliente con la direccion del servidor y la carpeta de destino,
//         y el texto con los argumentos "<tamano> <hash>" que siguen al comando.
// Devuelve: nada; imprime el resultado de la descarga o el error correspondiente.
static void atender_request_lookup(const contexto_cliente_p2p_t *contexto, const char *args) {
    uint64_t tamano;
    char hash[P2P_HASH_STR_LEN];
    char ruta_guardada[P2P_MAX_PATH];
    punto_red_p2p_t pares[P2P_MAX_PEERS];
    size_t cantidad_pares = 0;
    int ya_existe = 0;
    int segmentado = 0;

    if (leer_argumentos_request(args, &tamano, hash) != 0) {
        printf("uso: request <tamano> <hash>\n");
        return;
    }

    if (buscar_hash_en_servidor(contexto, tamano, hash, pares, P2P_MAX_PEERS, &cantidad_pares) != 0) {
        printf("fallo la busqueda del request\n");
        return;
    }

    if (cantidad_pares == 0) {
        printf("no se encontraron pares para %llu %s\n", (unsigned long long)tamano, hash);
        return;
    }

    printf("pares candidatos para %llu %s: %zu\n",
           (unsigned long long)tamano, hash, cantidad_pares);
    for (size_t i = 0; i < cantidad_pares; i++) {
        printf("%s %u\n", pares[i].ip, pares[i].puerto);
    }

    if (descargar_de_pares(contexto, pares, cantidad_pares, tamano, hash,
                                     ruta_guardada, sizeof(ruta_guardada),
                                     &ya_existe, &segmentado) != 0) {
        printf("fallo la descarga de %llu %s\n", (unsigned long long)tamano, hash);
        return;
    }

    if (ya_existe) {
        printf("descarga ya existe en %s\n", ruta_guardada);
    } else if (segmentado) {
        printf("descargado %llu %s en %s usando %zu pares\n",
               (unsigned long long)tamano, hash, ruta_guardada, cantidad_pares);
    } else {
        printf("descargado %llu %s en %s\n", (unsigned long long)tamano, hash, ruta_guardada);
    }
}

// Lee comandos del usuario en un bucle e invoca la accion correspondiente hasta que el usuario salga.
// Recibe: el contexto del cliente con toda la informacion de red y archivos locales necesaria para ejecutar los comandos.
// Devuelve: nada; termina cuando el usuario escribe "quit", "exit", o cierra stdin.
void correr_consola_cliente(contexto_cliente_p2p_t *contexto) {
    char linea[P2P_MAX_LINE];

    while (1) {
        printf("p2p> ");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            printf("\n");
            return;
        }

        linea[strcspn(linea, "\r\n")] = '\0';

        if (strcmp(linea, "quit") == 0 || strcmp(linea, "exit") == 0) {
            return;
        }
        if (strcmp(linea, "archivos") == 0) {
            mostrar_archivos(contexto);
            continue;
        }
        if (strcmp(linea, "vecinos") == 0) {
            if (actualizar_registro_servidor(contexto) != 0) {
                printf("no se pudo actualizar la lista de vecinos\n");
            }
            mostrar_vecinos(contexto);
            continue;
        }
        if (strncmp(linea, "find -s ", 8) == 0) {
            atender_find_servidor(contexto, linea + 8);
            continue;
        }
        if (strncmp(linea, "find -d ", 8) == 0) {
            if (actualizar_registro_servidor(contexto) != 0) {
                printf("no se pudo actualizar la lista de vecinos\n");
            }
            handle_find_distributed(contexto, linea + 8);
            continue;
        }
        if (strncmp(linea, "request ", 8) == 0) {
            atender_request_lookup(contexto, linea + 8);
            continue;
        }
        if (strncmp(linea, "find", 4) == 0 || strcmp(linea, "request") == 0) {
            printf("comando todavia no implementado\n");
            continue;
        }
        if (linea[0] != '\0') {
            printf("comando desconocido\n");
        }
    }
}
