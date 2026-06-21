#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdint.h>
#include "../protocol/protocol.h"

typedef struct {
    uint64_t high;
    uint64_t low;
} hash_p2p_t;

void iniciar_hash_p2p(hash_p2p_t *hash);
void actualizar_hash_p2p(hash_p2p_t *hash, const unsigned char *datos, size_t len);
void cerrar_hash_hex_p2p(const hash_p2p_t *hash, char out[P2P_HASH_STR_LEN]);
int calcular_hash_archivo(const char *ruta, char out[P2P_HASH_STR_LEN]);

#endif
