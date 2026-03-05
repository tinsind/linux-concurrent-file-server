#include "protocol.h"
#include "protocol_io.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define IO_BUF_SIZE 4096
#define LINE_BUF_SIZE 1024

static void trim_newline(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int connect_server(const char *ip, int port) {
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) {
        log_error("socket failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        log_error("invalid ip: %s", ip);
        close(cfd);
        return -1;
    }

    if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("connect failed");
        close(cfd);
        return -1;
    }

    return cfd;
}

static int handle_list_client(int cfd) {
    if (send_header(cfd, CMD_LIST, 0, 0) < 0) {
        return -1;
    }

    msg_header_t h;
    if (recv_header(cfd, &h) < 0) {
        return -1;
    }
    if (h.cmd != CMD_LIST || h.status != 0) {
        log_error("LIST failed, status=%u", h.status);
        return -1;
    }

    uint32_t left = h.length;
    char buf[IO_BUF_SIZE + 1];
    while (left > 0) {
        size_t chunk = left > IO_BUF_SIZE ? IO_BUF_SIZE : left;
        if (recv_all(cfd, buf, chunk) != (ssize_t)chunk) {
            return -1;
        }
        buf[chunk] = '\0';
        fputs(buf, stdout);
        left -= (uint32_t)chunk;
    }

    return 0;
}

static int handle_get_client(int cfd, const char *filename) {
    size_t fn_len = strlen(filename);
    if (fn_len == 0 || fn_len > 4096) {
        log_error("invalid filename");
        return -1;
    }

    if (send_header(cfd, CMD_GET, 0, (uint32_t)fn_len) < 0) {
        return -1;
    }
    if (send_all(cfd, filename, fn_len) != (ssize_t)fn_len) {
        return -1;
    }

    msg_header_t h;
    if (recv_header(cfd, &h) < 0) {
        return -1;
    }
    if (h.cmd != CMD_GET || h.status != 0) {
        log_error("GET failed, status=%u", h.status);
        return -1;
    }

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("open local file failed: %s", filename);

        char drop[IO_BUF_SIZE];
        uint32_t drop_left = h.length;
        while (drop_left > 0) {
            size_t chunk = drop_left > IO_BUF_SIZE ? IO_BUF_SIZE : drop_left;
            if (recv_all(cfd, drop, chunk) != (ssize_t)chunk) {
                return -1;
            }
            drop_left -= (uint32_t)chunk;
        }
        return -1;
    }

    char buf[IO_BUF_SIZE];
    uint32_t left = h.length;
    while (left > 0) {
        size_t chunk = left > IO_BUF_SIZE ? IO_BUF_SIZE : left;
        if (recv_all(cfd, buf, chunk) != (ssize_t)chunk) {
            close(fd);
            return -1;
        }

        size_t off = 0;
        while (off < chunk) {
            ssize_t n = write(fd, buf + off, chunk - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return -1;
            }
            off += (size_t)n;
        }

        left -= (uint32_t)chunk;
    }

    close(fd);
    log_info("GET ok: %s", filename);
    return 0;
}

static int handle_put_client(int cfd, const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_error("open local file failed: %s", filename);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 0xFFFFFFFFLL) {
        log_error("invalid local file: %s", filename);
        close(fd);
        return -1;
    }

    size_t fn_len = strlen(filename);
    if (fn_len == 0 || fn_len > 4096) {
        log_error("invalid filename");
        close(fd);
        return -1;
    }

    if (send_header(cfd, CMD_PUT, 0, (uint32_t)fn_len) < 0) {
        close(fd);
        return -1;
    }
    if (send_all(cfd, filename, fn_len) != (ssize_t)fn_len) {
        close(fd);
        return -1;
    }

    if (send_header(cfd, CMD_PUT, 0, (uint32_t)st.st_size) < 0) {
        close(fd);
        return -1;
    }

    char buf[IO_BUF_SIZE];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (send_all(cfd, buf, (size_t)n) != n) {
            close(fd);
            return -1;
        }
    }

    close(fd);

    msg_header_t ack;
    if (recv_header(cfd, &ack) < 0) {
        return -1;
    }
    if (ack.cmd != CMD_PUT || ack.status != 0) {
        log_error("PUT failed, status=%u", ack.status);
        return -1;
    }

    log_info("PUT ok: %s", filename);
    return 0;
}

static int handle_quit_client(int cfd) {
    if (send_header(cfd, CMD_QUIT, 0, 0) < 0) {
        return -1;
    }

    msg_header_t h;
    if (recv_header(cfd, &h) < 0) {
        return -1;
    }

    return (h.cmd == CMD_QUIT && h.status == 0) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }

    int cfd = connect_server(ip, port);
    if (cfd < 0) {
        return 1;
    }

    log_info("connected to %s:%d", ip, port);

    char line[LINE_BUF_SIZE];
    for (;;) {
        printf("client> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        char *cmd = strtok(line, " \t");
        char *arg = strtok(NULL, " \t");

        if (strcmp(cmd, "list") == 0) {
            if (handle_list_client(cfd) < 0) {
                log_error("list failed");
            }
        } else if (strcmp(cmd, "get") == 0) {
            if (!arg) {
                log_error("usage: get <filename>");
                continue;
            }
            if (handle_get_client(cfd, arg) < 0) {
                log_error("get failed");
            }
        } else if (strcmp(cmd, "put") == 0) {
            if (!arg) {
                log_error("usage: put <filename>");
                continue;
            }
            if (handle_put_client(cfd, arg) < 0) {
                log_error("put failed");
            }
        } else if (strcmp(cmd, "quit") == 0) {
            if (handle_quit_client(cfd) < 0) {
                log_error("quit handshake failed");
            }
            break;
        } else {
            log_error("unknown cmd: %s", cmd);
        }
    }

    close(cfd);
    return 0;
}
