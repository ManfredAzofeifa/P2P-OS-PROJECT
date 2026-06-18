#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "../protocol/protocol.h"
#include "../server/metadata.h"

#define CLIENT_MAX_LOCAL_FILES 1024

typedef struct {
    char server_ip[46];
    uint16_t server_port;
    uint16_t transfer_port;
    char shared_folder[P2P_MAX_PATH];
    p2p_file_metadata_t files[CLIENT_MAX_LOCAL_FILES];
    size_t file_count;
    p2p_endpoint_t neighbors[P2P_DEFAULT_NEIGHBORS];
    size_t neighbor_count;
} p2p_client_context_t;

int client_find_on_server(const p2p_client_context_t *context,
                          const char *name,
                          p2p_endpoint_t *peers,
                          size_t max_peers,
                          size_t *peer_count);
int client_lookup_on_server(const p2p_client_context_t *context,
                            uint64_t size,
                            const char *hash,
                            p2p_endpoint_t *peers,
                            size_t max_peers,
                            size_t *peer_count);
void client_run_console(p2p_client_context_t *context);

#endif
