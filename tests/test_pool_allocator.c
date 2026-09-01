/*
 * Standalone unit test for daemon/pool_allocator.c. No kernel, no VM, no
 * /dev/sensor0 -- runs directly on the host with the host's own cc.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../daemon/pool_allocator.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, \
				__LINE__);                                    \
			failures++;                                           \
		} else {                                                      \
			printf("ok: %s\n", msg);                              \
		}                                                             \
	} while (0)

typedef struct sample {
	uint64_t seq;
	uint64_t ts_ns;
	uint8_t raw;
} sample_t;

static void test_alloc_up_to_capacity(void)
{
	pool_allocator pool;
	void *slots[4];
	int i;

	CHECK(pool_init(&pool, sizeof(sample_t), 4) == 0, "pool_init succeeds");

	for (i = 0; i < 4; i++) {
		slots[i] = pool_alloc(&pool);
		CHECK(slots[i] != NULL, "alloc within capacity returns non-NULL");
	}

	/* all four slots must be distinct */
	CHECK(slots[0] != slots[1] && slots[0] != slots[2] &&
		      slots[0] != slots[3] && slots[1] != slots[2] &&
		      slots[1] != slots[3] && slots[2] != slots[3],
	      "the four allocated slots are pairwise distinct");

	pool_destroy(&pool);
}

static void test_exhaustion_returns_null(void)
{
	pool_allocator pool;
	void *a, *b, *c;

	CHECK(pool_init(&pool, sizeof(sample_t), 2) == 0, "pool_init succeeds (capacity 2)");

	a = pool_alloc(&pool);
	b = pool_alloc(&pool);
	CHECK(a != NULL && b != NULL, "both slots of a 2-capacity pool allocate");

	c = pool_alloc(&pool);
	CHECK(c == NULL, "a third alloc past capacity returns NULL, not a malloc fallback");

	pool_destroy(&pool);
}

static void test_free_then_realloc_reuses_slot(void)
{
	pool_allocator pool;
	void *a, *b;

	CHECK(pool_init(&pool, sizeof(sample_t), 1) == 0, "pool_init succeeds (capacity 1)");

	a = pool_alloc(&pool);
	CHECK(a != NULL, "single-capacity pool allocates its one slot");

	pool_free(&pool, a);
	b = pool_alloc(&pool);
	CHECK(a == b, "freeing then re-allocating returns the same slot (LIFO free list)");

	pool_destroy(&pool);
}

static void test_alloc_is_zeroed(void)
{
	pool_allocator pool;
	sample_t *s;

	CHECK(pool_init(&pool, sizeof(sample_t), 1) == 0, "pool_init succeeds (capacity 1)");

	s = pool_alloc(&pool);
	CHECK(s != NULL, "alloc succeeds");
	CHECK(s->seq == 0 && s->ts_ns == 0 && s->raw == 0,
	      "a freshly allocated slot is zeroed");

	/* dirty it, free it, re-alloc it: must come back zeroed again, not
	 * carrying stale bytes from the previous use. */
	s->seq = 0xdeadbeef;
	pool_free(&pool, s);
	s = pool_alloc(&pool);
	CHECK(s->seq == 0, "a re-allocated slot is re-zeroed, no stale data leaks across reuse");

	pool_destroy(&pool);
}

int main(void)
{
	test_alloc_up_to_capacity();
	test_exhaustion_returns_null();
	test_free_then_realloc_reuses_slot();
	test_alloc_is_zeroed();

	if (failures) {
		fprintf(stderr, "\n%d assertion(s) failed\n", failures);
		return 1;
	}

	printf("\nall assertions passed\n");
	return 0;
}
