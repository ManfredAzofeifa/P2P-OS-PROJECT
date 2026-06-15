/* console.c - Interfaz de consola: find y request */

#include "client.h"

#include <stdio.h>
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
        if (strncmp(line, "find", 4) == 0 || strncmp(line, "request ", 8) == 0) {
            printf("command not implemented in this component yet\n");
            continue;
        }
        if (line[0] != '\0') {
            printf("unknown command\n");
        }
    }
}
