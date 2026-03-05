#include "file_ops.h"
#include "protocol.h"
#include "protocol_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IO_BUF_SIZE 4096

static int is_safe_filename(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return 0;
    }

    if (strstr(filename, "..") != NULL) {
        return 0;
    }
    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
        return 0;
    }

    return 1;
}

static int write_all_fd(int fd, const void *buf, size_t len) {
    size_t off = 0;
    const char *p = (const char *)buf;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }

    return 0;
}

int handle_list(int cfd) {
    DIR *dir = opendir(".");
    if (!dir) {
        send_header(cfd, CMD_LIST, 1, 0);
        return -1;
    }

    char out[8192];
    size_t used = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        int n = snprintf(out + used, sizeof(out) - used, "%s\n", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(out) - used) {
            break;
        }
        used += (size_t)n;
    }
    closedir(dir);

    if (send_header(cfd, CMD_LIST, 0, (uint32_t)used) < 0) return -1;
    if (used > 0 && send_all(cfd, out, used) != (ssize_t)used) return -1;
    return 0;
}

int handle_get(int cfd, const char *filename) {
    if (!is_safe_filename(filename)) {
        return send_header(cfd, CMD_GET, 1, 0);
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return send_header(cfd, CMD_GET, 1, 0);
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 0xFFFFFFFFLL) {
        close(fd);
        return send_header(cfd, CMD_GET, 1, 0);
    }

    uint32_t file_len = (uint32_t)st.st_size;
    if (send_header(cfd, CMD_GET, 0, file_len) < 0) {
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
    return 0;
}

int handle_put(int cfd, const char *filename) {
    if (!is_safe_filename(filename)) {
        return send_header(cfd, CMD_PUT, 1, 0);
    }

    msg_header_t data_h;
    if (recv_header(cfd, &data_h) < 0) {
        return -1;
    }
    if (data_h.cmd != CMD_PUT) {
        send_header(cfd, CMD_PUT, 1, 0);
        return -1;
    }

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char buf[IO_BUF_SIZE];
        uint32_t left = data_h.length;
        while (left > 0) {
            size_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
            if (recv_all(cfd, buf, chunk) != (ssize_t)chunk) {
                return -1;
            }
            left -= (uint32_t)chunk;
        }
        send_header(cfd, CMD_PUT, 1, 0);
        return -1;
    }

    uint32_t left = data_h.length;
    char buf[IO_BUF_SIZE];
    while (left > 0) {
        size_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
        if (recv_all(cfd, buf, chunk) != (ssize_t)chunk) {
            close(fd);
            return -1;
        }
        if (write_all_fd(fd, buf, chunk) < 0) {
            close(fd);
            send_header(cfd, CMD_PUT, 1, 0);
            return -1;
        }
        left -= (uint32_t)chunk;
    }

    close(fd);
    return send_header(cfd, CMD_PUT, 0, 0);
}
