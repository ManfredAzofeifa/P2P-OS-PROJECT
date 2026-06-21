#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define P2P_MAX_LINE 1024
#define P2P_MAX_PATH 4096
#define P2P_MAX_NAME 256
#define P2P_HASH_HEX_LEN 32
#define P2P_HASH_STR_LEN (P2P_HASH_HEX_LEN + 1)
#define P2P_MAX_PEERS 32
#define P2P_DEFAULT_NEIGHBORS 4
#define P2P_DEFAULT_TTL 4
#define P2P_SEEN_QUERY_TTL_SECONDS 120
#define P2P_IO_TIMEOUT_SECONDS 3

/*
 * Socket protocol v1.
 *
 * Messages are ASCII lines terminated by '\n'. Numeric fields are decimal
 * except hashes, which are fixed-width lowercase hexadecimal strings.
 *
 * Client -> Server
 *   REGISTER <transfer_port> <file_count>
 *   FILE <size> <hash> <name>
 *   END
 *   FIND <name>
 *   LOOKUP <size> <hash>
 *
 * Server -> Client
 *   OK [text]
 *   ERROR <text>
 *   PEERS <count>
 *   PEER <ip> <puerto>
 *   END
 *   NEIGHBORS <count>
 *   NEIGHBOR <ip> <puerto>
 *
 * Client -> Client transfer
 *   GET <size> <hash> <offset> <length>
 *
 * Client -> Client transfer response
 *   DATA <length>
 *   <length raw bytes>
 *   ERROR <text>
 *
 * Client -> Client distributed search
 *   DSEARCH <query_id> <origin_ip> <origin_port> <ttl> <term>
 *   DRESULT <query_id> <size> <hash> <name> <owner_ip> <owner_port>
 */

typedef struct {
    char ip[46];
    uint16_t puerto;
} punto_red_p2p_t;

#endif
