/* console.c - Interfaz de consola: find y request */

#include "client.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_files(const p2p_client_context_t *context) {
    if (context->file_count == 0) {
        printf("no local files\n");
        return;
    }

    for (size_t i = 0; i < context->file_count; i++) {
        printf("%llu %s %s\n",
               (unsigned long long)context->files[i].size,
               context->files[i].hash,
               context->files[i].name);
    }
}

static void print_neighbors(const p2p_client_context_t *context) {
    if (context->neighbor_count == 0) {
        printf("no neighbors\n");
        return;
    }

    for (size_t i = 0; i < context->neighbor_count; i++) {
        printf("%s %u\n", context->neighbors[i].ip, context->neighbors[i].port);
    }
}

static void handle_find_server(const p2p_client_context_t *context, const char *name) {
    p2p_endpoint_t peers[P2P_MAX_PEERS];
    size_t peer_count = 0;

    if (name == NULL || name[0] == '\0') {
        printf("usage: find -s <name>\n");
        return;
    }

    if (client_find_on_server(context, name, peers, P2P_MAX_PEERS, &peer_count) != 0) {
        printf("server search failed\n");
        return;
    }

    if (peer_count == 0) {
        printf("no peers found for %s\n", name);
        return;
    }

    printf("peers for %s: %zu\n", name, peer_count);
    for (size_t i = 0; i < peer_count; i++) {
        printf("%s %u\n", peers[i].ip, peers[i].port);
    }
}

static int parse_request_args(const char *args, uint64_t *size, char hash[P2P_HASH_STR_LEN]) {
    char size_text[32];
    char hash_text[P2P_HASH_STR_LEN];
    unsigned long long parsed_size;
    char *end = NULL;

    if (args == NULL || size == NULL || hash == NULL) {
        return -1;
    }

    if (sscanf(args, "%31s %32s", size_text, hash_text) != 2) {
        return -1;
    }

    errno = 0;
    parsed_size = strtoull(size_text, &end, 10);
    if (errno != 0 || *size_text == '\0' || *end != '\0') {
        return -1;
    }

    if (strlen(hash_text) != P2P_HASH_HEX_LEN) {
        return -1;
    }

    *size = (uint64_t)parsed_size;
    snprintf(hash, P2P_HASH_STR_LEN, "%s", hash_text);
    return 0;
}

static void handle_request_lookup(const p2p_client_context_t *context, const char *args) {
    uint64_t size;
    char hash[P2P_HASH_STR_LEN];
    p2p_endpoint_t peers[P2P_MAX_PEERS];
    size_t peer_count = 0;

    if (parse_request_args(args, &size, hash) != 0) {
        printf("usage: request <size> <hash>\n");
        return;
    }

    if (client_lookup_on_server(context, size, hash, peers, P2P_MAX_PEERS, &peer_count) != 0) {
        printf("request lookup failed\n");
        return;
    }

    if (peer_count == 0) {
        printf("no peers found for %llu %s\n", (unsigned long long)size, hash);
        return;
    }

    printf("candidate peers for %llu %s: %zu\n",
           (unsigned long long)size, hash, peer_count);
    for (size_t i = 0; i < peer_count; i++) {
        printf("%s %u\n", peers[i].ip, peers[i].port);
    }
    printf("download not implemented in this component yet\n");
}

void client_run_console(p2p_client_context_t *context) {
    char line[P2P_MAX_LINE];

    while (1) {
        printf("p2p> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            return;
        }

        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            return;
        }
        if (strcmp(line, "files") == 0) {
            print_files(context);
            continue;
        }
        if (strcmp(line, "neighbors") == 0) {
            print_neighbors(context);
            continue;
        }
        if (strncmp(line, "find -s ", 8) == 0) {
            handle_find_server(context, line + 8);
            continue;
        }
        if (strncmp(line, "request ", 8) == 0) {
            handle_request_lookup(context, line + 8);
            continue;
        }
        if (strncmp(line, "find", 4) == 0 || strcmp(line, "request") == 0) {
            printf("command not implemented in this component yet\n");
            continue;
        }
        if (line[0] != '\0') {
            printf("unknown command\n");
        }
    }
}
