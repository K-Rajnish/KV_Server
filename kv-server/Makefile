CC = gcc
# Use pkg-config to supply include flags for libpq
PKG_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   libpq 2>/dev/null)

CFLAGS = -O2 -g -Wall -Wextra -pthread -std=gnu11 $(PKG_CFLAGS)
SRCS = src/main.c src/http.c src/cache.c src/db.c
BIN = kv_server

# civetweb library name: try -lcivetweb (package may be libcivetweb-dev) 
CIVET_LIB = -lcivetweb
LIBS = $(CIVET_LIB) $(PKG_LIBS)

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS) $(LIBS)

clean:
	rm -f $(BIN)
