CC ?= gcc
CFLAGS ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?= -pthread

.PHONY: all clean

SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)

all: static-server

static-server: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

clean:
	rm -f static-server
