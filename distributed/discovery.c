/* discovery.c - Protocolo de búsqueda distribuida */

#include "discovery.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DISTRIBUTED_RESULT_WAIT_MILLISECONDS 500

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t result_available;
    unsigned long long active_query_id;
    p2p_file_metadata_t *results;
    size_t max_results;
    size_t result_count;
    int active;
} distributed_query_state_t;

static distributed_query_state_t query_state = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    NULL,
    0,
    0,
    0
};
static unsigned long long next_query_id = 1;

static int write_all(int fd, const char *text) {
    size_t total = 0;
    size_t length = strlen(text);

    while (total < length) {
        ssize_t written = send(fd, text + total, length - total, 0);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)written;
    }
    return 0;
}

static int connect_to_endpoint(const char *host, uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port_text[16];
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_text, &hints, &addresses) != 0) {
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static void send_result(const p2p_client_context_t *context,
                        unsigned long long query_id,
                        const char *origin_ip,
                        uint16_t origin_port,
                        const p2p_file_metadata_t *file) {
    char line[P2P_MAX_LINE];
    int fd = connect_to_endpoint(origin_ip, origin_port);

    if (fd < 0) {
        return;
    }
    snprintf(line, sizeof(line), "DRESULT %llu %llu %s %s %s %u\n",
             query_id, (unsigned long long)file->size, file->hash, file->name,
             context->local_ip, context->transfer_port);
    write_all(fd, line);
    close(fd);
}

static int handle_search(const p2p_client_context_t *context, const char *line) {
    unsigned long long query_id;
    unsigned int origin_port;
    unsigned int ttl;
    char origin_ip[46];
    char term[P2P_MAX_NAME];

    if (sscanf(line, "DSEARCH %llu %45s %u %u %255s",
               &query_id, origin_ip, &origin_port, &ttl, term) != 5 ||
        origin_port == 0 || origin_port > 65535U || ttl == 0) {
        return 1;
    }

    for (size_t i = 0; i < context->file_count; i++) {
        if (strcmp(context->files[i].name, term) == 0) {
            send_result(context, query_id, origin_ip, (uint16_t)origin_port,
                        &context->files[i]);
        }
    }
    return 1;
}

static int handle_result(const char *line) {
    unsigned long long query_id;
    unsigned long long size;
    unsigned int owner_port;
    char hash[P2P_HASH_STR_LEN];
    char name[P2P_MAX_NAME];
    char owner_ip[46];

    if (sscanf(line, "DRESULT %llu %llu %32s %255s %45s %u",
               &query_id, &size, hash, name, owner_ip, &owner_port) != 6 ||
        strlen(hash) != P2P_HASH_HEX_LEN ||
        owner_port == 0 || owner_port > 65535U) {
        return 1;
    }

    pthread_mutex_lock(&query_state.mutex);
    if (query_state.active &&
        query_state.active_query_id == query_id &&
        query_state.result_count < query_state.max_results) {
        p2p_file_metadata_t *result;

        result = &query_state.results[query_state.result_count++];
        memset(result, 0, sizeof(*result));
        result->size = (uint64_t)size;
        snprintf(result->hash, sizeof(result->hash), "%s", hash);
        snprintf(result->name, sizeof(result->name), "%s", name);
        snprintf(result->owner.ip, sizeof(result->owner.ip), "%s", owner_ip);
        result->owner.port = (uint16_t)owner_port;
        pthread_cond_signal(&query_state.result_available);
    }
    pthread_mutex_unlock(&query_state.mutex);
    return 1;
}

int distributed_handle_peer_message(const p2p_client_context_t *context,
                                    const char *line) {
    if (context == NULL || line == NULL) {
        return 0;
    }
    if (strncmp(line, "DSEARCH ", 8) == 0) {
        return handle_search(context, line);
    }
    if (strncmp(line, "DRESULT ", 8) == 0) {
        return handle_result(line);
    }
    return 0;
}

int distributed_search_neighbors(const p2p_client_context_t *context,
                                 const char *term,
                                 p2p_file_metadata_t *results,
                                 size_t max_results,
                                 size_t *result_count) {
    struct timespec deadline;
    unsigned long long query_id;
    size_t sent = 0;

    if (context == NULL || term == NULL || results == NULL ||
        result_count == NULL || term[0] == '\0' ||
        strlen(term) >= P2P_MAX_NAME || strchr(term, '\n') != NULL) {
        return -1;
    }
    *result_count = 0;

    pthread_mutex_lock(&query_state.mutex);
    query_id = next_query_id++;
    query_state.active_query_id = query_id;
    query_state.results = results;
    query_state.max_results = max_results;
    query_state.result_count = 0;
    query_state.active = 1;
    pthread_mutex_unlock(&query_state.mutex);

    for (size_t i = 0; i < context->neighbor_count; i++) {
        char line[P2P_MAX_LINE];
        int fd = connect_to_endpoint(context->neighbors[i].ip,
                                     context->neighbors[i].port);

        if (fd < 0) {
            continue;
        }
        snprintf(line, sizeof(line), "DSEARCH %llu %s %u %u %s\n",
                 query_id, context->local_ip, context->transfer_port,
                 P2P_DEFAULT_TTL, term);
        if (write_all(fd, line) == 0) {
            sent++;
        }
        close(fd);
    }

    if (sent > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += DISTRIBUTED_RESULT_WAIT_MILLISECONDS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&query_state.mutex);
        while (pthread_cond_timedwait(&query_state.result_available,
                                      &query_state.mutex, &deadline) == 0) {
        }
        pthread_mutex_unlock(&query_state.mutex);
    }

    pthread_mutex_lock(&query_state.mutex);
    *result_count = query_state.result_count;
    query_state.active = 0;
    query_state.results = NULL;
    pthread_mutex_unlock(&query_state.mutex);
    return 0;
}
