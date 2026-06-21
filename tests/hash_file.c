#include <stdio.h>

#include "../server/hash.h"

int main(int argc, char **argv) {
    char hash[P2P_HASH_STR_LEN];

    if (argc != 2) {
        fprintf(stderr, "uso: %s <ruta>\n", argv[0]);
        return 2;
    }

    if (calcular_hash_archivo(argv[1], hash) != 0) {
        perror("calcular_hash_archivo");
        return 1;
    }

    printf("%s\n", hash);
    return 0;
}
