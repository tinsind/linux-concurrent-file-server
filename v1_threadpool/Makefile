CC ?= gcc
CROSS_COMPILE ?= aarch64-none-linux-gnu-
SERVER_CC = $(CROSS_COMPILE)$(CC)

CFLAGS := -Wall -Wextra -g -Iinclude
LDFLAGS := -lpthread

SERVER_SRCS := src/main.c src/server.c src/threadpool.c src/protocol.c src/file_ops.c src/log.c
CLIENT_SRCS := src/client.c src/protocol.c src/log.c

SERVER_BIN_DIR := server
CLIENT_BIN_DIR := client

SERVER_HOST_BIN := $(SERVER_BIN_DIR)/server_host
SERVER_A64_BIN := $(SERVER_BIN_DIR)/server_aarch64
CLIENT_HOST_BIN := $(CLIENT_BIN_DIR)/client_host
CLIENT_A64_BIN := $(CLIENT_BIN_DIR)/client_aarch64

BUILD_HOST := build/host
BUILD_A64 := build/aarch64

SERVER_OBJS_HOST := $(SERVER_SRCS:src/%.c=$(BUILD_HOST)/%.o)
SERVER_OBJS_A64 := $(SERVER_SRCS:src/%.c=$(BUILD_A64)/%.o)
CLIENT_OBJS_HOST := $(CLIENT_SRCS:src/%.c=$(BUILD_HOST)/%.o)
CLIENT_OBJS_A64 := $(CLIENT_SRCS:src/%.c=$(BUILD_A64)/%.o)

.PHONY: all clean server_host server_aarch64 client_host client_aarch64

all: server_host

server_host: $(SERVER_HOST_BIN)

server_aarch64: $(SERVER_A64_BIN)

client_host: $(CLIENT_HOST_BIN)

client_aarch64: $(CLIENT_A64_BIN)

$(SERVER_HOST_BIN): $(SERVER_OBJS_HOST)
	@mkdir -p $(dir $@)
	$(CC) $(SERVER_OBJS_HOST) -o $@ $(LDFLAGS)

$(SERVER_A64_BIN): $(SERVER_OBJS_A64)
	@mkdir -p $(dir $@)
	$(SERVER_CC) $(SERVER_OBJS_A64) -o $@ $(LDFLAGS)

$(CLIENT_HOST_BIN): $(CLIENT_OBJS_HOST)
	@mkdir -p $(dir $@)
	$(CC) $(CLIENT_OBJS_HOST) -o $@ $(LDFLAGS)

$(CLIENT_A64_BIN): $(CLIENT_OBJS_A64)
	@mkdir -p $(dir $@)
	$(SERVER_CC) $(CLIENT_OBJS_A64) -o $@ $(LDFLAGS)

$(BUILD_HOST)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_A64)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(SERVER_CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(SERVER_HOST_BIN) $(SERVER_A64_BIN) $(CLIENT_HOST_BIN) $(CLIENT_A64_BIN) server_host server_aarch64 client_host client_aarch64
