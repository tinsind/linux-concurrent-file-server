#ifndef __PROTOCOL_IO_H__
#define __PROTOCOL_IO_H__

#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include "protocol.h"

ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);

int send_header(int fd, uint16_t cmd, uint16_t status, uint32_t length);
int recv_header(int fd, msg_header_t *header);



#endif // __PROTOCOL_IO_H__
