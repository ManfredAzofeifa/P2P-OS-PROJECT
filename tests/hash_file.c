#include <stdio.h>

#include "../server/hash.h"

int main(int argc, char **argv) {
    char hash[P2P_HASH_STR_LEN];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <path>\n", argv[0]);
        return 2;
    }

    if (p2p_hash_file(argv[1], hash) != 0) {
        perror("p2p_hash_file");
        return 1;
    }

    printf("%s\n", hash);
    return 0;
}
