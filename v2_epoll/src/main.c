#include "server.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: 1-65535\n");
        return EXIT_FAILURE;
    }

    log_info("Starting epoll server on %s:%d", ip, port);
    return run_server(ip, port);
}
