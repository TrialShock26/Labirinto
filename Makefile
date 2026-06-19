CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
LDFLAGS = -pthread

# Directories
SERVER_DIR = server
CLIENT_DIR = client
COMMON_DIR = common

# Server sources
SERVER_SRCS = $(SERVER_DIR)/server.c \
              $(SERVER_DIR)/maze.c   \
              $(SERVER_DIR)/player.c \
              $(SERVER_DIR)/game.c   \
              $(SERVER_DIR)/logger.c

# Client sources
CLIENT_SRCS = $(CLIENT_DIR)/client.c

# Targets
SERVER_BIN = $(SERVER_DIR)/server
CLIENT_BIN = $(CLIENT_DIR)/client

.PHONY: all clean

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRCS) $(COMMON_DIR)/protocol.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_SRCS) $(COMMON_DIR)/protocol.h
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS) $(LDFLAGS)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
