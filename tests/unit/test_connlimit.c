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

/* Concurrency: many threads race for a small allowance on the same key. */
#define RACE_THREADS 8
#define RACE_ATTEMPTS 500
#define RACE_LIMIT 50u

static dx_connlimit_t *g_race_cl;
static _Atomic unsigned g_won;

static void *race_worker(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < RACE_ATTEMPTS; i++) {
        if (dx_connlimit_acquire(g_race_cl, kA, 4u, RACE_LIMIT)) {
            atomic_fetch_add(&g_won, 1u);
        }
    }
    return NULL;
}

static void test_concurrent_limit(void)
{
    pthread_t th[RACE_THREADS];

    dx_test_begin("connlimit/concurrent");

    g_race_cl = dx_connlimit_create(64u);
    atomic_store(&g_won, 0u);
    CHECK(g_race_cl != NULL);

    for (unsigned i = 0; i < RACE_THREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, race_worker, NULL) == 0);
    }
    for (unsigned i = 0; i < RACE_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    /* The whole point: exactly the limit is granted, never one more, no matter
     * how the threads interleave. */
    CHECK_EQ(atomic_load(&g_won), RACE_LIMIT);
    CHECK_EQ(dx_connlimit_count(g_race_cl, kA, 4u), (uint64_t)RACE_LIMIT);
    CHECK_EQ(dx_connlimit_refused(g_race_cl),
             (uint64_t)(RACE_THREADS * RACE_ATTEMPTS) - RACE_LIMIT);

    /* Release them all: the counter must return to zero exactly. */
    for (unsigned i = 0; i < RACE_LIMIT; i++) {
        dx_connlimit_release(g_race_cl, kA, 4u);
    }
    CHECK_EQ(dx_connlimit_count(g_race_cl, kA, 4u), 0u);
    CHECK_EQ(dx_connlimit_total(g_race_cl), 0u);

    dx_connlimit_destroy(g_race_cl);
}

int main(void)
{
    test_disabled_and_unlimited();
    test_limit_exact();
    test_stray_release_does_not_underflow();
    test_ipv6_keys();
    test_concurrent_limit();
    printf("test_connlimit: ");
    return dx_test_summary();
}
