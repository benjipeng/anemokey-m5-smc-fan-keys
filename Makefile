CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS ?= -framework IOKit

.PHONY: all clean

all: build/anemokey

build/anemokey: src/anemokey.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/anemokey.c $(LDFLAGS)

clean:
	rm -rf build
