#include "../include/protocol_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>




ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = (const char *)buf;
    while(sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if(n < 0) {
            if(errno == EINTR) continue; // Interrupted, try again
            return -1; // Error
        }
        if(n == 0 ) {
            return -1; // Connection closed
        }
        sent += n;
    }
    return (ssize_t )sent;
}

ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t recvd = 0;
    char *p = (char *)buf;
    while(recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if(n < 0) {
            if(errno == EINTR) continue; // Interrupted, try again
            return -1; // Error
        }
        if(n == 0) {
            return -1;
        }
        recvd += n;
    }

    return (ssize_t)recvd;
}

int send_header(int fd, uint16_t cmd, uint16_t status, uint32_t length) {
    msg_header_t net;
    net.magic = htonl(MAGIC_NUMBER);
    net.cmd = htons(cmd);
    net.status = htons(status);
    net.length = htonl(length);

    return (send_all(fd, &net, sizeof(net)) == (ssize_t)sizeof(net) ? 0 : -1);

}

int recv_header(int fd, msg_header_t *header) {
    msg_header_t net;
    if(recv_all(fd, &net, sizeof(net)) != (ssize_t)sizeof(net)) {
        return -1;
    }
    header->magic = ntohl(net.magic);
    header->cmd = ntohs(net.cmd);
    header->status = ntohs(net.status);
    header->length = ntohl(net.length);

    if(header->magic != MAGIC_NUMBER) {
        return -1; // Invalid magic number
    }
    
    return 0;
}
