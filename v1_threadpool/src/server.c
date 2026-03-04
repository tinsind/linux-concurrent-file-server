#include "server.h"
#include "threadpool.h"
#include "protocol.h"
#include "protocol_io.h"
#include "file_ops.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static void drain_payload(int cfd, uint32_t len) {
    char buf[1024];
    uint32_t left = len;

    while(left > 0) {
        size_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
        if(recv_all(cfd, buf, chunk) != chunk) {
            break;
        }
        left -= (uint32_t)chunk;
    }
}

static int recv_filename(int cfd, uint32_t len, char **out_name) {
    char *name = (char *)malloc(len + 1);
    if (!name) return -1;

    if (recv_all(cfd, name, len) != (ssize_t)len) {
        free(name);
        return -1;
    }
    name[len] = '\0';
    *out_name = name;
    return 0;
}

static void client_handler(client_task_t *task) {
    int cfd = task->fd;
    log_client_event("client connected", &task->addr);

    for (;;) {
        msg_header_t h;
        if (recv_header(cfd, &h) < 0) {
            break;
        }

        if (h.cmd == CMD_LIST) {
            if (h.length > 0) drain_payload(cfd, h.length);
            if (handle_list(cfd) < 0) {
                log_error("handle_list failed");
                break;
            }
        } else if (h.cmd == CMD_GET || h.cmd == CMD_PUT) {
            char *filename = NULL;
            if (h.length == 0 || recv_filename(cfd, h.length, &filename) < 0) {
                send_header(cfd, h.cmd, 1, 0);
                break;
            }

            int rc = (h.cmd == CMD_GET) ? handle_get(cfd, filename) : handle_put(cfd, filename);
            free(filename);

            if (rc < 0) {
                log_error("file op failed cmd=%u", h.cmd);
                break;
            }
        } else if (h.cmd == CMD_QUIT) {
            if (h.length > 0) drain_payload(cfd, h.length);
            send_header(cfd, CMD_QUIT, 0, 0);
            break;
        } else {
            if (h.length > 0) drain_payload(cfd, h.length);
            send_header(cfd, h.cmd, 1, 0);
        }
    }

    log_client_event("client disconnected", &task->addr);
    close(cfd);
}

int run_server(const char *ip, int port, int nthreads) {
    int lfd = -1;
    int opt = 1;
    struct sockaddr_in serv_addr;
    thread_pool_t *pool = NULL;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        log_error("socket failed");
        return 1;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) != 1) {
        log_error("invalid ip: %s", ip);
        close(lfd);
        return 1;
    }

    if (bind(lfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        log_error("bind failed");
        close(lfd);
        return 1;
    }

    if (listen(lfd, 128) < 0) {
        log_error("listen failed");
        close(lfd);
        return 1;
    }
    pool = thread_pool_create(nthreads, client_handler);
    if (!pool) {
        log_error("thread_pool_create failed");
        close(lfd);
        return 1;
    }

    log_info("listening on %s:%d", ip, port);

    for (;;) {
        client_task_t task;
        socklen_t len = sizeof(task.addr);

        task.fd = accept(lfd, (struct sockaddr *)&task.addr, &len);
        if (task.fd < 0) {
            if (errno == EINTR) continue;
            log_error("accept failed");
            continue;
        }

        if (thread_pool_submit(pool, &task) < 0) {
            log_error("thread_pool_submit failed");
            close(task.fd);
        }
    }

    thread_pool_destroy(pool);
    close(lfd);
    return 0;
}