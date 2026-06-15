#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdint.h>
#include "../protocol/protocol.h"

typedef struct {
    uint64_t high;
    uint64_t low;
} p2p_hash_t;

void p2p_hash_init(p2p_hash_t *hash);
void p2p_hash_update(p2p_hash_t *hash, const unsigned char *data, size_t len);
void p2p_hash_final_hex(const p2p_hash_t *hash, char out[P2P_HASH_STR_LEN]);
int p2p_hash_file(const char *path, char out[P2P_HASH_STR_LEN]);

#endif
