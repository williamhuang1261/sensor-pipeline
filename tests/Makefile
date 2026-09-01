CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c11

all: run

test_pool_allocator: test_pool_allocator.c ../daemon/pool_allocator.c ../daemon/pool_allocator.h
	$(CC) $(CFLAGS) -o test_pool_allocator test_pool_allocator.c ../daemon/pool_allocator.c

run: test_pool_allocator
	./test_pool_allocator

clean:
	rm -f test_pool_allocator

.PHONY: all run clean
