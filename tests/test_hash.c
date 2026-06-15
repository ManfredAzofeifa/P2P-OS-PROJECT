#include <stdio.h>
#include <string.h>

#include "../server/hash.h"

int main(int argc, char **argv) {
    char first[P2P_HASH_STR_LEN];
    char renamed[P2P_HASH_STR_LEN];
    char changed[P2P_HASH_STR_LEN];

    if (argc != 4) {
        fprintf(stderr, "usage: %s <same-content-a> <same-content-b> <different-content>\n", argv[0]);
        return 2;
    }

    if (p2p_hash_file(argv[1], first) != 0 ||
        p2p_hash_file(argv[2], renamed) != 0 ||
        p2p_hash_file(argv[3], changed) != 0) {
        perror("p2p_hash_file");
        return 1;
    }

    if (strlen(first) != P2P_HASH_HEX_LEN) {
        fprintf(stderr, "hash length mismatch: %zu\n", strlen(first));
        return 1;
    }

    if (strcmp(first, renamed) != 0) {
        fprintf(stderr, "same content produced different hashes\n");
        return 1;
    }

    if (strcmp(first, changed) == 0) {
        fprintf(stderr, "different content produced the same hash\n");
        return 1;
    }

    printf("%s\n", first);
    return 0;
}
