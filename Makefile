CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS ?= -framework IOKit

.PHONY: all clean

all: build/anemokey

SRCS = src/main.c src/probe.c src/hold.c src/smc.c

build/anemokey: $(SRCS) src/smc.h src/probe.h src/hold.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -rf build
