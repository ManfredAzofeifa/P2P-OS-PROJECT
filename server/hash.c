/* hash.c - Funcion de hash propia basada en el contenido del archivo. */

#include "hash.h"

#include <errno.h>
#include <stdio.h>

#define P2P_HASH_OFFSET_A 1469598103934665603ULL
#define P2P_HASH_OFFSET_B 1099511628211ULL
#define P2P_HASH_PRIME_A 1099511628211ULL
#define P2P_HASH_PRIME_B 14029467366897019727ULL

static uint64_t rotl64(uint64_t value, unsigned int bits) {
    return (value << bits) | (value >> (64U - bits));
}

void p2p_hash_init(p2p_hash_t *hash) {
    if (hash == NULL) {
        return;
    }

    hash->high = P2P_HASH_OFFSET_A;
    hash->low = P2P_HASH_OFFSET_B;
}

void p2p_hash_update(p2p_hash_t *hash, const unsigned char *data, size_t len) {
    size_t i;

    if (hash == NULL || data == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        unsigned char byte = data[i];

        hash->high ^= (uint64_t)byte;
        hash->high *= P2P_HASH_PRIME_A;
        hash->high = rotl64(hash->high, 13);

        hash->low += (uint64_t)byte + (hash->high & 0xffU);
        hash->low ^= rotl64(hash->low, 17);
        hash->low *= P2P_HASH_PRIME_B;
    }
}

void p2p_hash_final_hex(const p2p_hash_t *hash, char out[P2P_HASH_STR_LEN]) {
    if (hash == NULL || out == NULL) {
        return;
    }

    snprintf(out, P2P_HASH_STR_LEN, "%016llx%016llx",
             (unsigned long long)hash->high,
             (unsigned long long)hash->low);
}

int p2p_hash_file(const char *path, char out[P2P_HASH_STR_LEN]) {
    FILE *file;
    p2p_hash_t hash;
    unsigned char buffer[8192];
    size_t nread;

    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    p2p_hash_init(&hash);
    while ((nread = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        p2p_hash_update(&hash, buffer, nread);
    }

    if (ferror(file)) {
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        return -1;
    }

    p2p_hash_final_hex(&hash, out);
    return 0;
}
