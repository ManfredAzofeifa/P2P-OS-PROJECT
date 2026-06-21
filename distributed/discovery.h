#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stddef.h>
#include <time.h>
#include "../client/client.h"

int distributed_handle_peer_message(const p2p_client_context_t *context,
                                    const char *line);
int distributed_handle_peer_message_at(const p2p_client_context_t *context,
                                       const char *line,
                                       time_t now);
int distributed_search_neighbors(const p2p_client_context_t *context,
                                 const char *term, p2p_file_metadata_t *results,
                                 size_t max_results, size_t *result_count);

#endif
