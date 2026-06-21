#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../distributed/discovery.h"

static int create_capture_listener(uint16_t *port) {
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int fd;
    int flags;
    int enabled = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int read_forwarded_line(int listen_fd, char *line, size_t line_size) {
    int fd = accept(listen_fd, NULL, NULL);
    size_t used = 0;

    if (fd < 0) {
        return -1;
    }
    while (used + 1 < line_size) {
        ssize_t received = recv(fd, line + used, 1, 0);

        if (received <= 0) {
            break;
        }
        if (line[used++] == '\n') {
            break;
        }
    }
    line[used] = '\0';
    close(fd);
    return used > 0 ? 0 : -1;
}

static int expect_no_forward(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);

    if (fd >= 0) {
        close(fd);
        return -1;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "discovery test failed: %s\n", message);
        exit(1);
    }
}

int main(void) {
    p2p_client_context_t context;
    uint16_t capture_port;
    char line[P2P_MAX_LINE];
    int listen_fd;
    time_t first_seen = 1000;

    memset(&context, 0, sizeof(context));
    listen_fd = create_capture_listener(&capture_port);
    require(listen_fd >= 0, "cannot create capture listener");
    snprintf(context.neighbors[0].ip, sizeof(context.neighbors[0].ip),
             "127.0.0.1");
    context.neighbors[0].port = capture_port;
    context.neighbor_count = 1;

    require(distributed_handle_peer_message_at(
                &context,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen) == 1,
            "TTL search was not handled");
    require(read_forwarded_line(listen_fd, line, sizeof(line)) == 0,
            "TTL search was not forwarded");
    require(strcmp(line,
                   "DSEARCH 900 127.0.0.1 49999 2 needle\n") == 0,
            "forwarded TTL was not decremented");

    require(distributed_handle_peer_message_at(
                &context,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen + 1) == 1,
            "duplicate search was not handled");
    require(expect_no_forward(listen_fd) == 0,
            "duplicate query was forwarded");

    require(distributed_handle_peer_message_at(
                &context,
                "DSEARCH 901 127.0.0.1 49999 1 needle",
                first_seen + 2) == 1,
            "TTL-expired search was not handled");
    require(expect_no_forward(listen_fd) == 0,
            "TTL-expired search was forwarded");

    require(distributed_handle_peer_message_at(
                &context,
                "DSEARCH 900 127.0.0.1 49999 3 needle",
                first_seen + P2P_SEEN_QUERY_TTL_SECONDS) == 1,
            "expired seen query was not handled");
    require(read_forwarded_line(listen_fd, line, sizeof(line)) == 0,
            "expired seen query was not accepted again");

    close(listen_fd);
    printf("distributed discovery ok\n");
    return 0;
}
