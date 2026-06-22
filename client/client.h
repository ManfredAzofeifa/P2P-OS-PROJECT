#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "../protocol/protocol.h"
#include "../server/metadata.h"

#define CLIENT_MAX_LOCAL_FILES 1024

typedef struct {
    char ip_servidor[46];
    char ip_local[46];
    int usar_ip_anunciada;
    uint16_t puerto_servidor;
    uint16_t puerto_transferencia;
    char carpeta_compartida[P2P_MAX_PATH];
    metadato_archivo_p2p_t archivos[CLIENT_MAX_LOCAL_FILES];
    size_t cantidad_archivos;
    punto_red_p2p_t vecinos[P2P_DEFAULT_NEIGHBORS];
    size_t cantidad_vecinos;
} contexto_cliente_p2p_t;

/*
 * Publica nuevamente los archivos locales y reemplaza la lista de vecinos.
 * Recibe el contexto inicializado y devuelve 0 si completo el protocolo REGISTER.
 */
int actualizar_registro_servidor(contexto_cliente_p2p_t *contexto);

/*
 * Busca por nombre en el indice central.
 * Recibe el contexto, nombre, arreglo de salida, capacidad y contador de salida.
 * Devuelve 0 al recibir una respuesta completa (incluso vacia) o -1 ante error.
 */
int buscar_en_servidor(const contexto_cliente_p2p_t *contexto,
                          const char *nombre,
                          punto_red_p2p_t *pares,
                          size_t max_pares,
                          size_t *cantidad_pares);

/*
 * Busca por tamano y hash en el indice central para obtener fuentes de descarga.
 * Recibe el contexto, identidad del archivo, arreglo de salida, capacidad y contador.
 * Devuelve 0 al recibir una respuesta completa (incluso vacia) o -1 ante error.
 */
int buscar_hash_en_servidor(const contexto_cliente_p2p_t *contexto,
                            uint64_t tamano,
                            const char *hash,
                            punto_red_p2p_t *pares,
                            size_t max_pares,
                            size_t *cantidad_pares);

/*
 * Ejecuta la consola interactiva del cliente usando el contexto ya inicializado.
 * Recibe un contexto mutable y retorna cuando stdin cierra o se solicita salir.
 */
void correr_consola_cliente(contexto_cliente_p2p_t *contexto);

#endif
