#ifndef TRANSFER_H
#define TRANSFER_H

#include "client.h"

int transfer_server_start(const p2p_client_context_t *context);
int transfer_download_from_peer(const p2p_client_context_t *context,
                                const p2p_endpoint_t *peer,
                                uint64_t size,
                                const char *hash,
                                char *saved_path,
                                size_t saved_path_size,
                                int *already_present);

#endif
