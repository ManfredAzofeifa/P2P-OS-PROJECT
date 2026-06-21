#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include "../protocol/protocol.h"

typedef struct {
    char nombre[P2P_MAX_NAME];
    char hash[P2P_HASH_STR_LEN];
    uint64_t tamano;
    punto_red_p2p_t dueno;
} metadato_archivo_p2p_t;

#endif
