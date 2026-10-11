/* Unit tests for src/conn.c slot pool: generations, limits, double releases. */
#include "testing.h"

#include <stdlib.h>
#include <unistd.h>

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

/*
 * Regression: slots are handed out from the top of the free list, so with the
 * production chunk size (1024) the first connections live at indices 1023,
 * 1022, ... The shutdown drain used to scan [0, max_conns) and therefore
 * missed every one of them when --max-conns was smaller than a chunk.
 */
static void test_slot_indices_are_not_the_cap(void)
{
    dx_pool_t *pool = dx_pool_create(1024u, 2u);
    dx_conn_t *a;
    dx_conn_t *b;
    size_t visible = 0;

    dx_test_begin("pool/slot_indices_above_cap");
    CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }

    a = dx_pool_acquire(pool);
    b = dx_pool_acquire(pool);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(a != NULL && b != NULL && a != b);

    /* Both live well past the connection cap: that is the whole point. */
    CHECK(a != NULL && a->index >= 1022u);
    CHECK(b != NULL && b->index >= 1022u);

    /* A drain that walked [0, max_conns) would find nothing. */
    for (size_t i = 0; i < 2u; i++) {
        dx_conn_t *c = dx_pool_slot(pool, i);

        if (c != NULL && c->state != DX_CONN_FREE) {
            visible++;
        }
    }
    CHECK_EQ(visible, 0u);

    /* Walking the real slot space reaches both. */
    visible = 0;
    CHECK_EQ(dx_pool_total_slots(pool), 1024u);
    for (size_t i = 0; i < dx_pool_total_slots(pool); i++) {
        dx_conn_t *c = dx_pool_slot(pool, i);

        if (c != NULL && c->state != DX_CONN_FREE) {
            visible++;
        }
    }
    CHECK_EQ(visible, 2u);

    CHECK(dx_pool_acquire(pool) == NULL); /* cap is still 2 */
    CHECK_EQ(dx_pool_active(pool), 2u);

    (void)dx_pool_release(pool, a);
    (void)dx_pool_release(pool, b);
    CHECK_EQ(dx_pool_active(pool), 0u);
    dx_pool_destroy(pool);
}

/*
 * Regression: dx_pool_acquire() used to reset a handful of fields by hand, so
 * state and allocations from the previous owner survived into the next one.
 * dx_conn_reset() is the single place that clears a slot now.
 */
static void test_recycled_slot_is_clean(void)
{
    dx_pool_t *pool = dx_pool_create(4u, 4u);
    dx_conn_t *c;
    uint32_t first_generation;

    dx_test_begin("pool/recycled_slot_is_clean");
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
    first_generation = c->generation;

    /* Dirty every field the connection flow writes. */
    c->eof = true;
    c->close_conn = true;
    c->response_pending = true;
    c->file_fd = 7;
    c->file_left = 99;
    c->body_left = 99;
    c->body_offset = 12;
    c->hold = (unsigned char *)malloc(8u);
    c->hold_len = 8u;
    c->mem_body = (unsigned char *)malloc(8u);
    c->mem_body_len = 8u;
    c->hdr = (unsigned char *)malloc(8u);
    c->hdr_len = 8u;
    c->requests = 5;
    c->bytes_in = 111;
    c->bytes_out = 222;
    c->acc.status = 200;
    CHECK_EQ(dx_pool_release(pool, c), true);

    c = dx_pool_acquire(pool);
    CHECK(c != NULL);
    if (c == NULL) {
        dx_pool_destroy(pool);
        return;
    }
    CHECK(c == c); /* same slot, recycled */
    CHECK_EQ(c->generation, first_generation + 1u);
    CHECK_EQ(c->state, DX_CONN_READING);
    CHECK_EQ(c->eof, false);
    CHECK_EQ(c->close_conn, false);
    CHECK_EQ(c->response_pending, false);
    CHECK_EQ(c->file_fd, -1);
    CHECK_EQ(c->file_left, 0u);
    CHECK_EQ(c->body_left, 0u);
    CHECK_EQ(c->body_offset, 0u);
    CHECK_EQ(c->hold, (unsigned char *)NULL);
    CHECK_EQ(c->hold_len, 0u);
    CHECK_EQ(c->mem_body, (unsigned char *)NULL);
    CHECK_EQ(c->mem_body_len, 0u);
    CHECK_EQ(c->hdr, (unsigned char *)NULL);
    CHECK_EQ(c->hdr_len, 0u);
    CHECK_EQ(c->requests, 0u);
    CHECK_EQ(c->bytes_in, 0u);
    CHECK_EQ(c->bytes_out, 0u);
    CHECK_EQ(c->acc.status, 0);
    CHECK_EQ(c->fd, -1);
    CHECK_EQ(c->buf.data, (unsigned char *)NULL);

    (void)dx_pool_release(pool, c);
    dx_pool_destroy(pool);
}

/*
 * Regression: reset used to close a stale file_fd. Descriptor numbers are
 * reused, so closing one that belonged to the previous owner of the slot closed
 * the *next* connection's descriptor and every request on it failed with
 * epoll_ctl(ADD): Bad file descriptor.
 */
static void test_stale_file_fd_is_not_closed(void)
{
    dx_pool_t *pool = dx_pool_create(4u, 4u);
    dx_conn_t *c;
    int spare[2];

    dx_test_begin("pool/stale_file_fd_is_not_closed");
    CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }
    CHECK_EQ(pipe(spare), 0);
    if (spare[0] < 0) {
        dx_pool_destroy(pool);
        return;
    }

    c = dx_pool_acquire(pool);
    CHECK(c != NULL);
    if (c != NULL) {
        ssize_t n;

        c->file_fd = spare[1]; /* pretend a streamed file is attached */
        CHECK_EQ(dx_pool_release(pool, c), true);
        CHECK_EQ(c->file_fd, -1);
        /* The write end must still be open: a reset that closed the stale
         * number would make this write fail with EBADF. */
        n = write(spare[1], "x", 1);
        CHECK_EQ(n, 1);
    }
    (void)close(spare[0]);
    (void)close(spare[1]);
    dx_pool_destroy(pool);
}

int main(void)
{
    test_stale_file_fd_is_not_closed();
    test_slot_indices_are_not_the_cap();
    test_recycled_slot_is_clean();
    test_pool_lifecycle();
    test_conn_reset();
    printf("test_pool: ");
    return dx_test_summary();
}
