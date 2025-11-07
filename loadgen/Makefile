CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -std=gnu11 -pthread
PKG_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null)
PKG_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null)
LIBS = -lcurl

all: loadgen

loadgen: src/loadgen.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o loadgen src/loadgen.c $(PKG_LIBS)

clean:
	rm -f loadgen
