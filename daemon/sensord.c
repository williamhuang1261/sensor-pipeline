/*
 * sensord: reads lines from the sensor_i2c character device and decodes
 * them into Sample structs drawn from a fixed-size memory pool.
 *
 * The per-sample hot path (the read/parse/print loop below) never calls
 * malloc or free: pool_init() makes the pool's single, one-time backing
 * allocation before the loop starts, and every sample after that comes
 * from pool_alloc()/pool_free() -- see pool_allocator.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "pool_allocator.h"

#define DEFAULT_DEVICE "/dev/sensor0"
#define DEFAULT_SAMPLE_COUNT 20
#define POOL_CAPACITY 16 /* in-flight samples never exceed one at a time in this loop,
                           * sized headroom-generously to also work for a batched
                           * consumer without changing the allocator */

typedef struct sample {
	uint64_t seq;
	uint64_t ts_ns;
	uint8_t raw;
} sample_t;

static int parse_sample_line(const char *line, sample_t *out)
{
	unsigned long long seq, ts_ns;
	unsigned int raw;

	if (sscanf(line, "seq=%llu ts_ns=%llu raw=0x%02x", &seq, &ts_ns, &raw) != 3)
		return -1;

	out->seq = (uint64_t)seq;
	out->ts_ns = (uint64_t)ts_ns;
	out->raw = (uint8_t)raw;
	return 0;
}

int main(int argc, char **argv)
{
	const char *device = DEFAULT_DEVICE;
	int sample_count = DEFAULT_SAMPLE_COUNT;
	int fd;
	pool_allocator pool;
	char linebuf[128];
	int n;

	if (argc > 1)
		device = argv[1];
	if (argc > 2)
		sample_count = atoi(argv[2]);

	if (pool_init(&pool, sizeof(sample_t), POOL_CAPACITY) != 0) {
		fprintf(stderr, "sensord: pool_init failed\n");
		return 1;
	}

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "sensord: open(%s): %s\n", device, strerror(errno));
		pool_destroy(&pool);
		return 1;
	}

	fprintf(stderr, "sensord: reading %d samples from %s\n", sample_count, device);

	for (n = 0; n < sample_count; n++) {
		ssize_t r;
		size_t len = 0;

		/* /dev/sensor0's read() hands back exactly one formatted line
		 * per call (see driver/sensor_i2c.c:sensor_read), so a single
		 * read() into linebuf is a complete sample -- no line
		 * buffering/reassembly needed. */
		r = read(fd, linebuf, sizeof(linebuf) - 1);
		if (r < 0) {
			fprintf(stderr, "sensord: read: %s\n", strerror(errno));
			break;
		}
		if (r == 0)
			break;

		len = (size_t)r;
		linebuf[len] = '\0';

		sample_t *s = pool_alloc(&pool);
		if (!s) {
			fprintf(stderr, "sensord: pool exhausted, dropping a sample\n");
			continue;
		}

		if (parse_sample_line(linebuf, s) != 0) {
			fprintf(stderr, "sensord: unparsable line: %s", linebuf);
			pool_free(&pool, s);
			continue;
		}

		printf("sample #%d: seq=%llu ts_ns=%llu raw=0x%02x\n", n,
		       (unsigned long long)s->seq, (unsigned long long)s->ts_ns,
		       s->raw);

		pool_free(&pool, s);
	}

	close(fd);
	pool_destroy(&pool);
	return 0;
}
