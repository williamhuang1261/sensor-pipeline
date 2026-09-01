#ifndef POOL_ALLOCATOR_H
#define POOL_ALLOCATOR_H

#include <stddef.h>

/*
 * A fixed-size free-list memory pool.
 *
 * One backing buffer is allocated up front with a single malloc() call in
 * pool_init(). After that, pool_alloc()/pool_free() never call malloc/free:
 * free slots are linked into a singly-linked free list by writing a "next"
 * pointer into the unused slot's own memory (the classic intrusive
 * free-list trick), so handing out or reclaiming a slot is O(1) pointer
 * arithmetic with no heap traffic. This is the technique to keep a sampling
 * hot path free of allocator jitter/fragmentation when items are a fixed,
 * known size, as they are here (one Sample struct per reading).
 */
typedef struct pool_allocator {
	void *backing;         /* the single malloc'd buffer; freed by pool_destroy */
	void *free_list;       /* head of the intrusive free list, or NULL if exhausted */
	size_t item_size;      /* >= sizeof(void*), so a free slot can hold a next pointer */
	size_t capacity;
	size_t in_use;         /* items currently checked out, for pool_stats */
} pool_allocator;

/* item_size must be >= sizeof(void*). Returns 0 on success, -1 on malloc failure
 * or invalid arguments. */
int pool_init(pool_allocator *pool, size_t item_size, size_t capacity);

/* Returns a zeroed slot of item_size bytes, or NULL if the pool is exhausted.
 * Never calls malloc. */
void *pool_alloc(pool_allocator *pool);

/* Returns ptr (previously obtained from pool_alloc on the same pool) to the
 * free list. Never calls free. Undefined behavior if ptr was not obtained
 * from this pool or is freed twice. */
void pool_free(pool_allocator *pool, void *ptr);

/* Releases the single backing buffer. Do not use the pool afterwards. */
void pool_destroy(pool_allocator *pool);

#endif /* POOL_ALLOCATOR_H */
