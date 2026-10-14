/*
 * Unit tests for src/connlimit.c: the shared per-IP connection counter.
 *
 * The properties that matter:
 *   - the limit is enforced exactly, including concurrently;
 *   - a release returns the slot, so the counter tracks live connections;
 *   - a stray release never underflows into a permanent ban;
 *   - distinct addresses do not interfere, and a NULL/zero limit allows all.
 */
#include "testing.h"

#include "connlimit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Two distinct 4-byte keys. */
static const unsigned char kA[4] = {127, 0, 0, 1};
static const unsigned char kB[4] = {10, 0, 0, 7};

static void test_disabled_and_unlimited(void)
{
    dx_connlimit_t *cl;

    dx_test_begin("connlimit/disabled");

    /* NULL table and limit 0 both allow everything. */
    CHECK(dx_connlimit_acquire(NULL, kA, 4u, 5u));
    cl = dx_connlimit_create(64u);
    CHECK(cl != NULL);
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 0u)); /* 0 = unlimited */
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 0u));
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 0u); /* unlimited reserves nothing */
    CHECK_EQ(dx_connlimit_refused(cl), 0u);
    dx_connlimit_destroy(cl);
    dx_connlimit_destroy(NULL);

    /* A zero-slot table cannot be created. */
    CHECK(dx_connlimit_create(0u) == NULL);
}

static void test_limit_exact(void)
{
    dx_connlimit_t *cl = dx_connlimit_create(64u);

    dx_test_begin("connlimit/exact");

    CHECK(cl != NULL);
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(dx_connlimit_acquire(cl, kA, 4u, 3u));
    }
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 3u);

    /* The fourth is refused and does not change the count. */
    CHECK(!dx_connlimit_acquire(cl, kA, 4u, 3u));
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 3u);
    CHECK_EQ(dx_connlimit_refused(cl), 1u);

    /* Releasing one lets exactly one more in. */
    dx_connlimit_release(cl, kA, 4u);
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 2u);
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 3u));
    CHECK(!dx_connlimit_acquire(cl, kA, 4u, 3u));

    /* Another address is unaffected by A's limit. */
    CHECK(dx_connlimit_acquire(cl, kB, 4u, 3u));
    CHECK_EQ(dx_connlimit_count(cl, kB, 4u), 1u);

    CHECK_EQ(dx_connlimit_distinct(cl), 2u);
    CHECK_EQ(dx_connlimit_total(cl), 4u);

    dx_connlimit_destroy(cl);
}

static void test_stray_release_does_not_underflow(void)
{
    dx_connlimit_t *cl = dx_connlimit_create(64u);

    dx_test_begin("connlimit/no_underflow");

    CHECK(cl != NULL);
    /* Releasing without acquiring must not wrap the counter to a huge value:
     * that would deny the address forever. */
    dx_connlimit_release(cl, kA, 4u);
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 0u);
    dx_connlimit_release(cl, kA, 4u);
    dx_connlimit_release(cl, kA, 4u);
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 0u);

    /* The limit still works normally afterwards. */
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 2u));
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 2u));
    CHECK(!dx_connlimit_acquire(cl, kA, 4u, 2u));

    /* Releasing an address that was never tracked is a no-op. */
    dx_connlimit_release(cl, kB, 4u);
    CHECK_EQ(dx_connlimit_count(cl, kB, 4u), 0u);

    dx_connlimit_destroy(cl);
}

static void test_ipv6_keys(void)
{
    dx_connlimit_t *cl = dx_connlimit_create(64u);
    static const unsigned char v6a[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                          0,    0,    0,    0,    0, 0, 0, 1};
    static const unsigned char v6b[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                          0,    0,    0,    0,    0, 0, 0, 2};

    dx_test_begin("connlimit/ipv6");

    CHECK(cl != NULL);
    CHECK(dx_connlimit_acquire(cl, v6a, 16u, 2u));
    CHECK(dx_connlimit_acquire(cl, v6a, 16u, 2u));
    CHECK(!dx_connlimit_acquire(cl, v6a, 16u, 2u));
    /* A different v6 address is independent. */
    CHECK(dx_connlimit_acquire(cl, v6b, 16u, 2u));
    CHECK_EQ(dx_connlimit_count(cl, v6a, 16u), 2u);
    CHECK_EQ(dx_connlimit_count(cl, v6b, 16u), 1u);

    /* An unusable key length is refused by the table but must fail open: the
     * caller allows the connection rather than denying service. */
    CHECK(dx_connlimit_acquire(cl, v6a, 0u, 1u));
    CHECK(dx_connlimit_acquire(cl, v6a, 17u, 1u));

    dx_connlimit_destroy(cl);
}

/*
 * Concurrency: the slot claim must be atomic.
 *
 * The bug this targets: an empty slot was claimed by writing the key and then
 * checking whether it was still free. The loser had already overwritten the
 * winner's key bytes before its check failed, so both keys ended up sharing one
 * counter and one key's allowance silently doubled -- the exact failure the
 * limit exists to prevent.
 *
 * The signature is a mismatch between a key's own grant count and that key's
 * own counter. A start barrier makes the threads collide on the empty slots
 * instead of drifting apart, which is what makes this deterministic enough to
 * be a test rather than a coin flip.
 */
#define RACE_THREADS 16
#define RACE_ATTEMPTS 50000
#define RACE_KEYS 16
#define RACE_LIMIT 200u

static unsigned char g_keys[RACE_KEYS][4];
static dx_connlimit_t *g_race_cl;
static _Atomic unsigned g_grants[RACE_KEYS];
static pthread_barrier_t g_start_gate;

static void *race_worker(void *arg)
{
    int idx = *(int *)arg;

    pthread_barrier_wait(&g_start_gate);
    for (unsigned i = 0; i < RACE_ATTEMPTS; i++) {
        if (dx_connlimit_acquire(g_race_cl, g_keys[idx], 4u, RACE_LIMIT)) {
            atomic_fetch_add(&g_grants[idx], 1u);
        }
    }
    return NULL;
}

static void test_concurrent_slot_claim(void)
{
    pthread_t th[RACE_THREADS];
    static int id[RACE_THREADS];

    dx_test_begin("connlimit/concurrent_slot_claim");

    for (unsigned round = 0; round < 3u; round++) {
        for (int i = 0; i < RACE_KEYS; i++) {
            g_keys[i][0] = 10;
            g_keys[i][1] = 0;
            g_keys[i][2] = (unsigned char)i;
            g_keys[i][3] = 1;
        }
        g_race_cl = dx_connlimit_create((size_t)RACE_KEYS);
        CHECK(g_race_cl != NULL);
        for (int i = 0; i < RACE_KEYS; i++) {
            atomic_store(&g_grants[i], 0u);
        }
        pthread_barrier_init(&g_start_gate, NULL, RACE_THREADS);
        for (int i = 0; i < RACE_THREADS; i++) {
            id[i] = i;
            CHECK(pthread_create(&th[i], NULL, race_worker, &id[i]) == 0);
        }
        for (int i = 0; i < RACE_THREADS; i++) {
            pthread_join(th[i], NULL);
        }
        pthread_barrier_destroy(&g_start_gate);

        /* Every key's counter must equal that key's own grants. A mismatch means
         * two keys shared a counter because the claim raced. */
        for (int i = 0; i < RACE_KEYS; i++) {
            uint64_t counter = dx_connlimit_count(g_race_cl, g_keys[i], 4u);

            CHECK_EQ(counter, (uint64_t)atomic_load(&g_grants[i]));
        }
        dx_connlimit_destroy(g_race_cl);
        g_race_cl = NULL;
    }
}

/*
 * The table has a fixed number of slots, so a burst of distinct addresses can
 * exhaust it. The documented behaviour then is FAIL OPEN: an address that
 * cannot be tracked is allowed, never refused on suspicion, and no existing
 * counter is disturbed. That is the deliberate trade -- a connection limit is a
 * fairness control, not an access control, and refusing a legitimate client
 * because the table is busy would be a worse failure than not tracking it.
 *
 * The test uses a table of exactly DX_CL_SHARDS slots (the minimum the
 * constructor produces) so that a burst of distinct keys must exhaust it.
 */
static void test_full_table_fails_open(void)
{
    dx_connlimit_t *cl = dx_connlimit_create(16u);
    unsigned allowed = 0;
    unsigned char key[4];

    dx_test_begin("connlimit/full_table_fails_open");

    CHECK(cl != NULL);
    /* 40 distinct addresses against a 16-slot table, limit 1 each. */
    for (int i = 0; i < 40; i++) {
        key[0] = 10;
        key[1] = 0;
        key[2] = (unsigned char)(i >> 8);
        key[3] = (unsigned char)i;
        if (dx_connlimit_acquire(cl, key, 4u, 1u)) {
            allowed++;
        }
    }
    /* Every address is allowed: the ones that fit are tracked, the ones that do
     * not are let through rather than refused. */
    CHECK_EQ(allowed, 40u);
    /* And the table tracked exactly as many addresses as it has slots. */
    CHECK_EQ(dx_connlimit_distinct(cl), 16u);

    dx_connlimit_destroy(cl);
}

static void test_release_returns_to_zero(void)
{
    dx_connlimit_t *cl = dx_connlimit_create(64u);

    dx_test_begin("connlimit/release_returns_to_zero");

    CHECK(cl != NULL);
    for (unsigned i = 0; i < 10u; i++) {
        CHECK(dx_connlimit_acquire(cl, kA, 4u, 10u));
    }
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 10u);
    for (unsigned i = 0; i < 10u; i++) {
        dx_connlimit_release(cl, kA, 4u);
    }
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 0u);
    CHECK_EQ(dx_connlimit_total(cl), 0u);
    /* The slot stays claimed until it is reused, which is what distinct counts. */
    CHECK_EQ(dx_connlimit_distinct(cl), 1u);

    /* And the address can be acquired again from zero. */
    CHECK(dx_connlimit_acquire(cl, kA, 4u, 10u));
    CHECK_EQ(dx_connlimit_count(cl, kA, 4u), 1u);

    dx_connlimit_destroy(cl);
}

int main(void)
{
    test_disabled_and_unlimited();
    test_limit_exact();
    test_stray_release_does_not_underflow();
    test_ipv6_keys();
    test_full_table_fails_open();
    test_release_returns_to_zero();
    test_concurrent_slot_claim();
    printf("test_connlimit: ");
    return dx_test_summary();
}
