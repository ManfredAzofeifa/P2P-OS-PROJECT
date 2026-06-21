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
    uint16_t puerto_servidor;
    uint16_t puerto_transferencia;
    char carpeta_compartida[P2P_MAX_PATH];
    metadato_archivo_p2p_t archivos[CLIENT_MAX_LOCAL_FILES];
    size_t cantidad_archivos;
    punto_red_p2p_t vecinos[P2P_DEFAULT_NEIGHBORS];
    size_t cantidad_vecinos;
} contexto_cliente_p2p_t;

int buscar_en_servidor(const contexto_cliente_p2p_t *contexto,
                          const char *nombre,
                          punto_red_p2p_t *pares,
                          size_t max_pares,
                          size_t *cantidad_pares);
int buscar_hash_en_servidor(const contexto_cliente_p2p_t *contexto,
                            uint64_t tamano,
                            const char *hash,
                            punto_red_p2p_t *pares,
                            size_t max_pares,
                            size_t *cantidad_pares);
void correr_consola_cliente(contexto_cliente_p2p_t *contexto);

#endif
