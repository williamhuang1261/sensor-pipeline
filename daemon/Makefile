CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c11

all: sensord

sensord: sensord.c pool_allocator.c pool_allocator.h
	$(CC) $(CFLAGS) -o sensord sensord.c pool_allocator.c

clean:
	rm -f sensord

.PHONY: all clean
