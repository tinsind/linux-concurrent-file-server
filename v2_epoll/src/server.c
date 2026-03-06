#include "server.h"
#include "protocol.h"
#include "log.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define MAX_FILENAME_LEN 4096
#define MAX_FD_MAP 65536
#define IO_BUF_SIZE 4096
#define MAX_GENERIC_PAYLOAD (1024 * 1024)

typedef enum {
    ST_READ_HEADER = 0,
    ST_READ_PAYLOAD,
    ST_READ_PUT_STREAM,
    ST_WRITE_BUFFER,
    ST_WRITE_GET_FILE
} conn_state_t;

typedef struct {
    int fd;
    struct sockaddr_in addr;

    conn_state_t state;

    msg_header_t cur_header;
    uint8_t header_buf[sizeof(msg_header_t)];
    size_t header_read;

    uint8_t *payload;
    uint32_t payload_len;
    uint32_t payload_read;

    char *pending_put_name;
    int put_fd;
    uint32_t put_left;

    uint8_t *out_buf;
    size_t out_len;
    size_t out_sent;

    int get_fd;
    uint32_t get_left;

    bool close_after_write;
} conn_t;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static bool is_safe_filename(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return false;
    }
    if (strstr(filename, "..") != NULL) {
        return false;
    }
    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
        return false;
    }
    return true;
}

static int mod_events(int epfd, conn_t *c, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events | EPOLLRDHUP;
    ev.data.fd = c->fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static int set_interest_by_state(int epfd, conn_t *c) {
    switch (c->state) {
        case ST_READ_HEADER:
        case ST_READ_PAYLOAD:
        case ST_READ_PUT_STREAM:
            return mod_events(epfd, c, EPOLLIN);
        case ST_WRITE_BUFFER:
        case ST_WRITE_GET_FILE:
            return mod_events(epfd, c, EPOLLOUT);
        default:
            return -1;
    }
}

static void free_conn_buffers(conn_t *c) {
    if (c->payload) {
        free(c->payload);
        c->payload = NULL;
    }
    c->payload_len = 0;
    c->payload_read = 0;

    if (c->out_buf) {
        free(c->out_buf);
        c->out_buf = NULL;
    }
    c->out_len = 0;
    c->out_sent = 0;

    if (c->pending_put_name) {
        free(c->pending_put_name);
        c->pending_put_name = NULL;
    }

    if (c->put_fd >= 0) {
        close(c->put_fd);
        c->put_fd = -1;
    }

    if (c->get_fd >= 0) {
        close(c->get_fd);
        c->get_fd = -1;
    }
    c->get_left = 0;
    c->put_left = 0;
}

static void close_conn(int epfd, conn_t **conn_map, conn_t *c) {
    int fd = c->fd;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    log_client_event("client disconnected", &c->addr);

    if (fd >= 0 && fd < MAX_FD_MAP) {
        conn_map[fd] = NULL;
    }

    free_conn_buffers(c);
    free(c);
}

static int queue_reply(conn_t *c, uint16_t cmd, uint16_t status, const void *payload, uint32_t len) {
    msg_header_t net;
    uint8_t *buf = (uint8_t *)malloc(sizeof(msg_header_t) + len);
    if (!buf) {
        return -1;
    }

    net.magic = htonl(MAGIC_NUMBER);
    net.cmd = htons(cmd);
    net.status = htons(status);
    net.length = htonl(len);

    memcpy(buf, &net, sizeof(net));
    if (len > 0 && payload != NULL) {
        memcpy(buf + sizeof(net), payload, len);
    }

    if (c->out_buf) {
        free(c->out_buf);
    }
    c->out_buf = buf;
    c->out_len = sizeof(msg_header_t) + len;
    c->out_sent = 0;
    c->state = ST_WRITE_BUFFER;
    return 0;
}

static int build_list_payload(char **out, uint32_t *out_len) {
    DIR *dir = opendir(".");
    if (!dir) {
        return -1;
    }

    size_t cap = 8192;
    size_t used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        closedir(dir);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        size_t n = strlen(ent->d_name) + 1;
        if (used + n >= cap) {
            size_t new_cap = cap * 2;
            while (used + n >= new_cap) {
                new_cap *= 2;
            }
            char *tmp = (char *)realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                closedir(dir);
                return -1;
            }
            buf = tmp;
            cap = new_cap;
        }

        memcpy(buf + used, ent->d_name, n - 1);
        buf[used + n - 1] = '\n';
        used += n;
    }

    closedir(dir);
    *out = buf;
    *out_len = (uint32_t)used;
    return 0;
}

static int start_get_transfer(conn_t *c, const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return queue_reply(c, CMD_GET, 1, NULL, 0);
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 0xFFFFFFFFLL) {
        close(fd);
        return queue_reply(c, CMD_GET, 1, NULL, 0);
    }

    msg_header_t net;
    net.magic = htonl(MAGIC_NUMBER);
    net.cmd = htons(CMD_GET);
    net.status = htons(0);
    net.length = htonl((uint32_t)st.st_size);

    if (c->out_buf) {
        free(c->out_buf);
    }
    c->out_buf = (uint8_t *)malloc(sizeof(net));
    if (!c->out_buf) {
        close(fd);
        return -1;
    }

    memcpy(c->out_buf, &net, sizeof(net));
    c->out_len = sizeof(net);
    c->out_sent = 0;
    c->get_fd = fd;
    c->get_left = (uint32_t)st.st_size;
    c->state = ST_WRITE_BUFFER;
    return 0;
}

static int handle_request(conn_t *c) {
    uint16_t cmd = c->cur_header.cmd;

    if (cmd == CMD_LIST) {
        if (c->cur_header.length != 0) {
            c->close_after_write = true;
            return queue_reply(c, CMD_LIST, 1, NULL, 0);
        }

        char *list_payload = NULL;
        uint32_t list_len = 0;
        if (build_list_payload(&list_payload, &list_len) < 0) {
            return queue_reply(c, CMD_LIST, 1, NULL, 0);
        }

        int rc = queue_reply(c, CMD_LIST, 0, list_payload, list_len);
        free(list_payload);
        return rc;
    }

    if (cmd == CMD_GET) {
        if (c->cur_header.length == 0 || c->cur_header.length > MAX_FILENAME_LEN) {
            c->close_after_write = true;
            return queue_reply(c, CMD_GET, 1, NULL, 0);
        }

        char filename[MAX_FILENAME_LEN + 1];
        memcpy(filename, c->payload, c->cur_header.length);
        filename[c->cur_header.length] = '\0';
        if (!is_safe_filename(filename)) {
            c->close_after_write = true;
            return queue_reply(c, CMD_GET, 1, NULL, 0);
        }

        return start_get_transfer(c, filename);
    }

    if (cmd == CMD_PUT) {
        if (!c->pending_put_name) {
            if (c->cur_header.length == 0 || c->cur_header.length > MAX_FILENAME_LEN) {
                c->close_after_write = true;
                return queue_reply(c, CMD_PUT, 1, NULL, 0);
            }

            char *filename = (char *)malloc(c->cur_header.length + 1);
            if (!filename) {
                return -1;
            }
            memcpy(filename, c->payload, c->cur_header.length);
            filename[c->cur_header.length] = '\0';

            if (!is_safe_filename(filename)) {
                free(filename);
                c->close_after_write = true;
                return queue_reply(c, CMD_PUT, 1, NULL, 0);
            }

            c->pending_put_name = filename;
            c->state = ST_READ_HEADER;
            return 0;
        }

        c->put_fd = open(c->pending_put_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (c->put_fd < 0) {
            free(c->pending_put_name);
            c->pending_put_name = NULL;
            return queue_reply(c, CMD_PUT, 1, NULL, 0);
        }

        c->put_left = c->cur_header.length;
        c->state = ST_READ_PUT_STREAM;

        if (c->put_left == 0) {
            close(c->put_fd);
            c->put_fd = -1;
            free(c->pending_put_name);
            c->pending_put_name = NULL;
            return queue_reply(c, CMD_PUT, 0, NULL, 0);
        }

        return 0;
    }

    if (cmd == CMD_QUIT) {
        c->close_after_write = true;
        return queue_reply(c, CMD_QUIT, 0, NULL, 0);
    }

    return queue_reply(c, cmd, 1, NULL, 0);
}

static int handle_read_header(conn_t *c) {
    while (c->header_read < sizeof(msg_header_t)) {
        ssize_t n = recv(c->fd, c->header_buf + c->header_read, sizeof(msg_header_t) - c->header_read, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        c->header_read += (size_t)n;
    }

    msg_header_t net;
    memcpy(&net, c->header_buf, sizeof(net));

    c->cur_header.magic = ntohl(net.magic);
    c->cur_header.cmd = ntohs(net.cmd);
    c->cur_header.status = ntohs(net.status);
    c->cur_header.length = ntohl(net.length);

    c->header_read = 0;

    if (c->cur_header.magic != MAGIC_NUMBER) {
        return -1;
    }

    if (c->pending_put_name && c->cur_header.cmd != CMD_PUT) {
        c->close_after_write = true;
        return queue_reply(c, CMD_PUT, 1, NULL, 0);
    }

    if (c->pending_put_name && c->cur_header.cmd == CMD_PUT) {
        c->state = ST_READ_PUT_STREAM;
        c->put_left = c->cur_header.length;
        c->put_fd = open(c->pending_put_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (c->put_fd < 0) {
            free(c->pending_put_name);
            c->pending_put_name = NULL;
            return queue_reply(c, CMD_PUT, 1, NULL, 0);
        }

        if (c->put_left == 0) {
            close(c->put_fd);
            c->put_fd = -1;
            free(c->pending_put_name);
            c->pending_put_name = NULL;
            return queue_reply(c, CMD_PUT, 0, NULL, 0);
        }

        return 0;
    }

    if (c->cur_header.length == 0) {
        if (c->payload) {
            free(c->payload);
            c->payload = NULL;
        }
        c->payload_len = 0;
        c->payload_read = 0;
        return handle_request(c);
    }

    if (c->cur_header.length > MAX_FILENAME_LEN &&
        (c->cur_header.cmd == CMD_GET || c->cur_header.cmd == CMD_PUT)) {
        c->close_after_write = true;
        return queue_reply(c, c->cur_header.cmd, 1, NULL, 0);
    }

    if (c->cur_header.length > MAX_GENERIC_PAYLOAD) {
        c->close_after_write = true;
        return queue_reply(c, c->cur_header.cmd, 1, NULL, 0);
    }

    if (c->payload) {
        free(c->payload);
    }
    c->payload = (uint8_t *)malloc(c->cur_header.length);
    if (!c->payload) {
        return -1;
    }
    c->payload_len = c->cur_header.length;
    c->payload_read = 0;
    c->state = ST_READ_PAYLOAD;
    return 0;
}

static int handle_read_payload(conn_t *c) {
    while (c->payload_read < c->payload_len) {
        ssize_t n = recv(c->fd, c->payload + c->payload_read, c->payload_len - c->payload_read, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        c->payload_read += (uint32_t)n;
    }

    int rc = handle_request(c);
    free(c->payload);
    c->payload = NULL;
    c->payload_len = 0;
    c->payload_read = 0;
    return rc;
}

static int handle_put_stream(conn_t *c) {
    uint8_t buf[IO_BUF_SIZE];

    while (c->put_left > 0) {
        size_t chunk = c->put_left > IO_BUF_SIZE ? IO_BUF_SIZE : c->put_left;
        ssize_t n = recv(c->fd, buf, chunk, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(c->put_fd, buf + off, (size_t)n - off);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            off += (size_t)w;
        }

        c->put_left -= (uint32_t)n;
    }

    close(c->put_fd);
    c->put_fd = -1;

    free(c->pending_put_name);
    c->pending_put_name = NULL;

    return queue_reply(c, CMD_PUT, 0, NULL, 0);
}

static int handle_write_buffer(conn_t *c) {
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->fd, c->out_buf + c->out_sent, c->out_len - c->out_sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        c->out_sent += (size_t)n;
    }

    free(c->out_buf);
    c->out_buf = NULL;
    c->out_len = 0;
    c->out_sent = 0;

    if (c->get_fd >= 0 && c->get_left > 0) {
        c->state = ST_WRITE_GET_FILE;
        return 0;
    }

    if (c->close_after_write) {
        return -1;
    }

    c->state = ST_READ_HEADER;
    return 0;
}

static int handle_write_get_file(conn_t *c) {
    while (c->get_left > 0) {
        ssize_t n = sendfile(c->fd, c->get_fd, NULL, c->get_left);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        c->get_left -= (uint32_t)n;
    }

    if (c->get_left == 0) {
        close(c->get_fd);
        c->get_fd = -1;
        c->state = ST_READ_HEADER;
    }

    return 0;
}

int run_server(const char *ip, int port) {
    int lfd = -1;
    int epfd = -1;
    int opt = 1;
    struct sockaddr_in serv_addr;
    conn_t **conn_map = NULL;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        log_error("socket failed");
        return 1;
    }

    if (set_nonblocking(lfd) < 0) {
        log_error("set_nonblocking(listen) failed");
        close(lfd);
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

    if (listen(lfd, 1024) < 0) {
        log_error("listen failed");
        close(lfd);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        log_error("epoll_create1 failed");
        close(lfd);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        log_error("epoll_ctl add listen fd failed");
        close(epfd);
        close(lfd);
        return 1;
    }

    conn_map = (conn_t **)calloc(MAX_FD_MAP, sizeof(conn_t *));
    if (!conn_map) {
        log_error("alloc conn_map failed");
        close(epfd);
        close(lfd);
        return 1;
    }

    log_info("epoll server listening on %s:%d", ip, port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int nready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            if (fd == lfd) {
                for (;;) {
                    struct sockaddr_in cli_addr;
                    socklen_t len = sizeof(cli_addr);
                    int cfd = accept(lfd, (struct sockaddr *)&cli_addr, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        log_error("accept failed");
                        break;
                    }

                    if (cfd >= MAX_FD_MAP) {
                        close(cfd);
                        continue;
                    }

                    if (set_nonblocking(cfd) < 0) {
                        close(cfd);
                        continue;
                    }

                    conn_t *c = (conn_t *)calloc(1, sizeof(conn_t));
                    if (!c) {
                        close(cfd);
                        continue;
                    }

                    c->fd = cfd;
                    c->addr = cli_addr;
                    c->state = ST_READ_HEADER;
                    c->put_fd = -1;
                    c->get_fd = -1;
                    conn_map[cfd] = c;

                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        conn_map[cfd] = NULL;
                        free(c);
                        close(cfd);
                        continue;
                    }

                    log_client_event("client connected", &cli_addr);
                }
                continue;
            }

            if (fd < 0 || fd >= MAX_FD_MAP) {
                continue;
            }

            conn_t *c = conn_map[fd];
            if (!c) {
                continue;
            }

            if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_conn(epfd, conn_map, c);
                continue;
            }

            int rc = 0;
            if (revents & EPOLLIN) {
                if (c->state == ST_READ_HEADER) {
                    rc = handle_read_header(c);
                } else if (c->state == ST_READ_PAYLOAD) {
                    rc = handle_read_payload(c);
                } else if (c->state == ST_READ_PUT_STREAM) {
                    rc = handle_put_stream(c);
                }
            }

            if (rc == 0 && (revents & EPOLLOUT)) {
                if (c->state == ST_WRITE_BUFFER) {
                    rc = handle_write_buffer(c);
                } else if (c->state == ST_WRITE_GET_FILE) {
                    rc = handle_write_get_file(c);
                }
            }

            if (rc < 0) {
                close_conn(epfd, conn_map, c);
                continue;
            }

            if (set_interest_by_state(epfd, c) < 0) {
                close_conn(epfd, conn_map, c);
                continue;
            }
        }
    }

    for (int fd = 0; fd < MAX_FD_MAP; ++fd) {
        if (conn_map[fd]) {
            close_conn(epfd, conn_map, conn_map[fd]);
        }
    }

    free(conn_map);
    close(epfd);
    close(lfd);
    return 1;
}
