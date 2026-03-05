#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

#define MAGIC_NUMBER 0x12345678

#if defined(__GNUC__) || defined(__clang__)
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

typedef enum {
    CMD_LIST = 1,
    CMD_GET,
    CMD_PUT,
    CMD_QUIT
} command_t;

typedef struct PACKED {
    uint32_t magic;     // Magic number for validation
    uint16_t cmd;       // Command type
    uint16_t status;    // Status code (for responses)
    uint32_t length;    // Length of the payload

} msg_header_t;

_Static_assert(sizeof(msg_header_t) == 12, "msg_header_t size must be 12 bytes");

#undef PACKED



#endif // __PROTOCOL_H__
