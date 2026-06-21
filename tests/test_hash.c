#include <stdio.h>
#include <string.h>

#include "../server/hash.h"

int main(int argc, char **argv) {
    char first[P2P_HASH_STR_LEN];
    char renamed[P2P_HASH_STR_LEN];
    char changed[P2P_HASH_STR_LEN];

    if (argc != 4) {
        fprintf(stderr, "uso: %s <same-content-a> <same-content-b> <different-content>\n", argv[0]);
        return 2;
    }

    if (calcular_hash_archivo(argv[1], first) != 0 ||
        calcular_hash_archivo(argv[2], renamed) != 0 ||
        calcular_hash_archivo(argv[3], changed) != 0) {
        perror("calcular_hash_archivo");
        return 1;
    }

    if (strlen(first) != P2P_HASH_HEX_LEN) {
        fprintf(stderr, "hash largo mismatch: %zu\n", strlen(first));
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
