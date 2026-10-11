/* Unit tests for src/buffer.c (dx_buffer_t and the slot pool). */
#include "testing.h"

#include "buffer.h"

#include <string.h>

static void test_buffer_window(void)
{
    dx_buffer_t b;
    unsigned char mem[16];

    dx_test_begin("buffer/window");

    dx_buffer_init(&b, mem, sizeof(mem));
    CHECK_EQ(dx_buffer_readable(&b), 0);
    CHECK_EQ(dx_buffer_writable(&b), 16);

    /* Append and read back. */
    CHECK(dx_buffer_append(&b, "hello", 5));
    CHECK_EQ(dx_buffer_readable(&b), 5);
    CHECK_EQ(b.len, 5);
    CHECK_EQ(b.off, 0);
    CHECK(memcmp(b.data, "hello", 5) == 0);

    /* Consume part of the window; the offset must move, not the data. */
    dx_buffer_consume(&b, 2);
    CHECK_EQ(dx_buffer_readable(&b), 3);
    CHECK_EQ(b.off, 2);

    /* Consume more than available is clamped, never wraps. */
    dx_buffer_consume(&b, 100);
    CHECK_EQ(dx_buffer_readable(&b), 0);
    CHECK_EQ(b.off, 0);      /* fully drained resets the window */
    CHECK_EQ(b.len, 0);
    CHECK_EQ(dx_buffer_writable(&b), 16);

    /* Filling the buffer rejects an append that does not fit. */
    CHECK(dx_buffer_append(&b, mem, 16));
    CHECK(!dx_buffer_append(&b, "x", 1));
    CHECK_EQ(dx_buffer_readable(&b), 16);

    /* NULL and empty cases are safe. */
    CHECK(!dx_buffer_append(&b, NULL, 1));
    CHECK(dx_buffer_append(&b, "x", 0));
    dx_buffer_consume(&b, 16);

    /* Compaction moves live data to the front. */
    CHECK(dx_buffer_append(&b, "abc", 3));
    dx_buffer_consume(&b, 2);
    dx_buffer_compact(&b);
    CHECK_EQ(b.off, 0);
    CHECK_EQ(b.len, 1);
    CHECK_EQ(b.data[0], 'c');
}

static void test_pool_basics(void)
{
    dx_bufpool_t *pool;
    unsigned char *a;
    unsigned char *b;

    dx_test_begin("pool/basics");

    /* Invalid arguments are rejected instead of crashing later. */
    CHECK(dx_bufpool_create(0, 8) == NULL);
    CHECK(dx_bufpool_create(64, 0) == NULL);

    pool = dx_bufpool_create(64, 2);
    CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }

    CHECK_EQ(dx_bufpool_outstanding(pool), 0);
    CHECK_EQ(dx_bufpool_total_slots(pool), 0);

    a = dx_bufpool_acquire(pool, 64);
    CHECK(a != NULL);
    CHECK_EQ(dx_bufpool_outstanding(pool), 1);
    CHECK_EQ(dx_bufpool_total_slots(pool), 2);

    b = dx_bufpool_acquire(pool, 64);
    CHECK(b != NULL);
    CHECK(a != b);
    CHECK_EQ(dx_bufpool_outstanding(pool), 2);

    /* Pool is exhausted: the next acquire allocates a new chunk. */
    {
        unsigned char *c = dx_bufpool_acquire(pool, 64);
        CHECK(c != NULL);
        CHECK_EQ(dx_bufpool_total_slots(pool), 4);
        CHECK(dx_bufpool_release(pool, c));
    }

    /* Oversized requests fail instead of overflowing a slot. */
    CHECK(dx_bufpool_acquire(pool, 65) == NULL);

    /* Ownership accounting is exact. */
    CHECK(dx_bufpool_release(pool, a));
    CHECK_EQ(dx_bufpool_outstanding(pool), 1);
    CHECK(!dx_bufpool_release(pool, a));       /* double release rejected */
    CHECK_EQ(dx_bufpool_outstanding(pool), 1);
    CHECK(!dx_bufpool_release(pool, a + 7));   /* not a slot base */
    CHECK(!dx_bufpool_release(pool, NULL));    /* defensive */
    CHECK_EQ(dx_bufpool_outstanding(pool), 1);

    /* A pointer from outside every chunk is rejected. */
    {
        unsigned char foreign[64];
        CHECK(!dx_bufpool_release(pool, foreign));
    }

    /* The released slot must come back with the same address. */
    CHECK(dx_bufpool_release(pool, b));
    CHECK_EQ(dx_bufpool_outstanding(pool), 0);
    {
        unsigned char *again = dx_bufpool_acquire(pool, 64);
        CHECK(again == a || again == b);       /* only two slots exist */
        (void)dx_bufpool_release(pool, again);
    }

    dx_bufpool_destroy(pool);
}

int main(void)
{
    test_buffer_window();
    test_pool_basics();
    printf("test_buffer: ");
    return dx_test_summary();
}
