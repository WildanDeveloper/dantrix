/* Unit tests for src/conn.c slot pool: generations, limits, double releases. */
#include "testing.h"

#include "conn.h"

#include <string.h>

static void test_pool_lifecycle(void)
{
    dx_pool_t *pool;

    dx_test_begin("pool/lifecycle");

    CHECK(dx_pool_create(0, 10) == NULL);
    CHECK(dx_pool_create(4, 0) == NULL);

    pool = dx_pool_create(4, 8);
    CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }
    CHECK_EQ(dx_pool_active(pool), 0);

    /* Acquire gives a slot with fd -1, a non-zero generation and READING. */
    {
        dx_conn_t *a = dx_pool_acquire(pool);
        uint32_t gen_a;

        CHECK(a != NULL);
        if (a == NULL) {
            dx_pool_destroy(pool);
            return;
        }
        CHECK_EQ(a->fd, -1);
        CHECK_EQ(a->state, DX_CONN_READING);
        CHECK(a->generation != 0);
        CHECK_EQ(dx_pool_active(pool), 1);
        gen_a = a->generation;

        /* Release returns the slot; the pointer stays valid but is marked FREE. */
        CHECK(dx_pool_release(pool, a));
        CHECK_EQ(a->state, DX_CONN_FREE);
        CHECK_EQ(dx_pool_active(pool), 0);
        CHECK(!dx_pool_release(pool, a)); /* double release rejected */

        /* Re-acquire must bump the generation so stale tags stop resolving. */
        {
            dx_conn_t *b = dx_pool_acquire(pool);

            CHECK(b != NULL);
            if (b != NULL) {
                CHECK_EQ(b->index, a->index);  /* same slot reused */
                CHECK(b->generation != gen_a); /* generation moved forward */
                CHECK(dx_pool_lookup(pool, b->index, b->generation) == b);
                CHECK(dx_pool_lookup(pool, b->index, gen_a) == NULL); /* stale */
                CHECK(dx_pool_lookup(pool, b->index, 0) == NULL);
                CHECK(dx_pool_lookup(pool, b->index + 1000u, b->generation) == NULL);
                dx_pool_release(pool, b);
            }
        }
    }

    /* The limit is enforced with NULL, never by handing out a shared slot. */
    {
        dx_conn_t *held[16];
        size_t i;
        size_t got = 0;

        for (i = 0; i < 16; i++) {
            held[i] = dx_pool_acquire(pool);
            if (held[i] != NULL) {
                got++;
                /* Every handed out slot must be distinct memory. */
                for (size_t j = 0; j < i; j++) {
                    CHECK(held[i] != held[j]);
                }
            }
        }
        CHECK_EQ(got, 8); /* max_conns */
        CHECK_EQ(dx_pool_active(pool), 8);
        for (i = 0; i < 16; i++) {
            if (held[i] != NULL) {
                CHECK(dx_pool_release(pool, held[i]));
            }
        }
        CHECK_EQ(dx_pool_active(pool), 0);
    }

    dx_pool_destroy(pool);
}

static void test_conn_reset(void)
{
    dx_pool_t *pool = dx_pool_create(4, 2);
    dx_conn_t *c;

    dx_test_begin("conn/reset");
    CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }
    c = dx_pool_acquire(pool);
    CHECK(c != NULL);
    if (c == NULL) {
        dx_pool_destroy(pool);
        return;
    }

    dx_conn_init(c, 42, NULL);
    CHECK_EQ(c->fd, 42);
    CHECK(dx_conn_has_pending(c) == false); /* nothing buffered yet */

    dx_conn_reset(c);
    CHECK_EQ(c->fd, -1);
    CHECK_EQ(c->buf.data, (unsigned char *)NULL);
    CHECK_EQ(c->buf_mem, (unsigned char *)NULL);

    dx_pool_release(pool, c);
    dx_pool_destroy(pool);
}

int main(void)
{
    test_pool_lifecycle();
    test_conn_reset();
    printf("test_pool: ");
    return dx_test_summary();
}
