CC ?= gcc
CROSS_COMPILE ?= aarch64-none-linux-gnu-
SERVER_CC = $(CROSS_COMPILE)$(CC)

CFLAGS := -Wall -Wextra -g -Iinclude
LDFLAGS := -lpthread

SRCS := src/main.c src/server.c src/threadpool.c src/protocol.c src/file_ops.c src/log.c

BUILD_HOST := build/host
BUILD_A64 := build/aarch64

OBJS_HOST := $(SRCS:src/%.c=$(BUILD_HOST)/%.o)
OBJS_A64 := $(SRCS:src/%.c=$(BUILD_A64)/%.o)

.PHONY: all clean server_host server_aarch64

all: server_host

server_host: $(OBJS_HOST)
	$(CC) $(OBJS_HOST) -o $@ $(LDFLAGS)

server_aarch64: $(OBJS_A64)
	$(SERVER_CC) $(OBJS_A64) -o $@ $(LDFLAGS)

$(BUILD_HOST)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_A64)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(SERVER_CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build server_host server_aarch64
