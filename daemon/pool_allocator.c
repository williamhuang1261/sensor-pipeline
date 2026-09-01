#include "pool_allocator.h"

#include <stdlib.h>
#include <string.h>

int pool_init(pool_allocator *pool, size_t item_size, size_t capacity)
{
	size_t i;
	char *base;

	if (!pool || item_size < sizeof(void *) || capacity == 0)
		return -1;

	base = malloc(item_size * capacity);
	if (!base)
		return -1;

	pool->backing = base;
	pool->item_size = item_size;
	pool->capacity = capacity;
	pool->in_use = 0;

	/* Thread each slot's first sizeof(void*) bytes into a singly-linked
	 * free list, tail (last slot) pointing to NULL. */
	pool->free_list = NULL;
	for (i = 0; i < capacity; i++) {
		void *slot = base + i * item_size;
		*(void **)slot = pool->free_list;
		pool->free_list = slot;
	}

	return 0;
}

void *pool_alloc(pool_allocator *pool)
{
	void *slot;

	if (!pool || !pool->free_list)
		return NULL; /* exhausted: the caller decides how to react, we never malloc */

	slot = pool->free_list;
	pool->free_list = *(void **)slot;
	pool->in_use++;

	memset(slot, 0, pool->item_size);
	return slot;
}

void pool_free(pool_allocator *pool, void *ptr)
{
	if (!pool || !ptr)
		return;

	*(void **)ptr = pool->free_list;
	pool->free_list = ptr;
	pool->in_use--;
}

void pool_destroy(pool_allocator *pool)
{
	if (!pool)
		return;

	free(pool->backing);
	pool->backing = NULL;
	pool->free_list = NULL;
	pool->capacity = 0;
	pool->in_use = 0;
}
