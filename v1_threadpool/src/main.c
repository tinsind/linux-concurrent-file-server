#include "server.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if(argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <threads>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int threads = atoi(argv[3]);
    if(port <= 0 || port > 65535 || threads <= 0) {
        fprintf(stderr, "Invalid port number: port(1-65535), threads(>0)\n");
        return EXIT_FAILURE;
    }

    log_info("Starting server on %s:%d with %d threads", ip, port, threads);
    return run_server(ip, port, threads);
}
