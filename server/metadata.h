#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include "../protocol/protocol.h"

typedef struct {
    char name[P2P_MAX_NAME];
    char hash[P2P_HASH_STR_LEN];
    uint64_t size;
    p2p_endpoint_t owner;
} p2p_file_metadata_t;

#endif
