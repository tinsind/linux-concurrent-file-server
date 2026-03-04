#include "file_ops.h"
#include "protocol.h"
#include "protocol_io.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

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
    (void)filename;
    return send_header(cfd, CMD_GET, 2, 0);
}

int handle_put(int cfd, const char *filename) {
    (void)filename;
    return send_header(cfd, CMD_PUT, 2, 0);
}
