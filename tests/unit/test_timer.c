/*
 * Unit tests for src/timer.c: the hashed timing wheel.
 *
 * Deadlines are absolute, so the tests drive a fake clock and check that a
 * timer fires exactly when due, never early, and that refreshing, cancelling
 * and the computed epoll timeout stay consistent.
 */
#include "testing.h"

#include "timer.h"

#include <string.h>

typedef struct {
    size_t fired;
    uint64_t last_deadline;
} fire_log_t;

static void on_expire(void *ctx, dx_timer_node_t *node)
{
    fire_log_t *log = (fire_log_t *)ctx;

    log->fired++;
    log->last_deadline = node->deadline_ms;
}

typedef struct {
    dx_timer_wheel_t *wheel;
    dx_timer_node_t *victim;
    size_t fired;
} mutation_ctx_t;

typedef struct {
    dx_timer_wheel_t *wheel;
    dx_timer_node_t *node;
    uint64_t now_ms;
    size_t count;
} rearm_ctx_t;

/* Re-arms the node it is given with a later deadline. */
static void rearm_cb(void *ctx, dx_timer_node_t *node)
{
    rearm_ctx_t *c = (rearm_ctx_t *)ctx;

    (void)node;
    c->count++;
    (void)dx_timer_wheel_refresh(c->wheel, c->node, 5000u, c->now_ms);
}

static void on_expire_noop(void *ctx, dx_timer_node_t *node)
{
    rearm_ctx_t *c = (rearm_ctx_t *)ctx;

    (void)node;
    c->count++;
}

/* Cancels another node of the same bucket while the wheel is firing one. */
static void cancel_other_cb(void *ctx, dx_timer_node_t *node)
{
    mutation_ctx_t *c = (mutation_ctx_t *)ctx;

    (void)node;
    c->fired++;
    dx_timer_wheel_cancel(c->wheel, c->victim);
}

static void test_start_and_cancel(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    dx_timer_node_t b;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/start_cancel");
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    dx_timer_node_reset(&a);
    dx_timer_node_reset(&b);

    CHECK_EQ(dx_timer_wheel_active(w), 0u);
    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    CHECK_EQ(dx_timer_node_deadline(&a), 1000u);

    CHECK_EQ(dx_timer_wheel_start(w, &b, 2000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_active(w), 2u);

    /* Cancel removes it from the wheel and clears the deadline. */
    dx_timer_wheel_cancel(w, &a);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    CHECK_EQ(dx_timer_node_deadline(&a), 0u);
    CHECK(a.active == false);

    /* Cancelling twice, or cancelling an unarmed node, is harmless. */
    dx_timer_wheel_cancel(w, &a);
    dx_timer_wheel_cancel(w, &b);
    CHECK_EQ(dx_timer_wheel_active(w), 0u);

    /* A cancelled timer never fires. */
    CHECK_EQ(dx_timer_wheel_advance(w, 5000u, on_expire, &log), 0u);
    CHECK_EQ(log.fired, 0u);

    dx_timer_wheel_destroy(w);
}

static void test_fires_at_deadline(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    dx_timer_node_t b;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/fires_at_deadline");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);
    dx_timer_node_reset(&b);

    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_start(w, &b, 3000u, 0u), 0);

    /* Nothing is due before the first deadline. */
    CHECK_EQ(dx_timer_wheel_advance(w, 999u, on_expire, &log), 0u);
    CHECK_EQ(log.fired, 0u);
    /* Exactly at the deadline it fires. */
    CHECK_EQ(dx_timer_wheel_advance(w, 1000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 1u);
    CHECK_EQ(log.last_deadline, 1000u);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    /* The later timer is untouched. */
    CHECK_EQ(dx_timer_wheel_advance(w, 1500u, on_expire, &log), 0u);
    /* A big jump fires everything that came due, in one call. */
    CHECK_EQ(dx_timer_wheel_advance(w, 10000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 2u);
    CHECK_EQ(dx_timer_wheel_active(w), 0u);

    dx_timer_wheel_destroy(w);
}

static void test_same_bucket_not_fired_early(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    dx_timer_node_t b;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/same_bucket");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);
    dx_timer_node_reset(&b);

    /*
     * Both land in the bucket for second 5 (deadlines 5000 and 5500). Firing
     * the bucket must respect the absolute deadlines: only the first one may
     * go at 5000, even though the wheel inspects that bucket once per tick.
     */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 5000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_start(w, &b, 5500u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_advance(w, 5000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 1u);
    CHECK_EQ(log.last_deadline, 5000u);
    /* The other one is still armed and not due yet. */
    CHECK_EQ(dx_timer_wheel_advance(w, 5499u, on_expire, &log), 0u);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    /* It fires when its own deadline arrives. */
    CHECK_EQ(dx_timer_wheel_advance(w, 5500u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 2u);
    CHECK_EQ(log.last_deadline, 5500u);

    dx_timer_wheel_destroy(w);
}

static void test_refresh(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/refresh");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);

    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 0u), 0);
    /* Refreshing an unarmed node arms it. */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 2000u, 500u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 2500u);

    /* Same bucket: the deadline moves without touching the list. */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 2000u, 600u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 2600u);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);

    /* A refresh that shortens the deadline past "now" makes it due. */
    CHECK_EQ(dx_timer_wheel_advance(w, 3000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 1u);

    /* A refresh past one bucket boundary moves the node. */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 10000u), 0);
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 5000u, 10000u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 15000u);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    CHECK_EQ(dx_timer_wheel_advance(w, 14999u, on_expire, &log), 0u);
    CHECK_EQ(dx_timer_wheel_advance(w, 15000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 2u);
    CHECK_EQ(log.last_deadline, 15000u);

    /* Refreshing a node that already fired arms it again. */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 1000u, 20000u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 21000u);
    CHECK_EQ(dx_timer_wheel_active(w), 1u);
    CHECK_EQ(dx_timer_wheel_advance(w, 21000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 3u);

    dx_timer_wheel_destroy(w);
}

static void test_next_timeout(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    dx_timer_node_t b;

    dx_test_begin("timer/next_timeout");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);
    dx_timer_node_reset(&b);

    /* Empty wheel: no deadline to wait for. */
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 0u), (int)DX_TIMER_NO_TIMEOUT);

    CHECK_EQ(dx_timer_wheel_start(w, &a, 5000u, 1000u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 6000u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 1000u), 5000);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 5000u), 1000);

    /* The earlier of several deadlines wins. */
    CHECK_EQ(dx_timer_wheel_start(w, &b, 1000u, 1000u), 0);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 1000u), 1000);

    /* A due deadline yields zero, never a negative value. */
    CHECK_EQ(dx_timer_wheel_start(w, &b, 50000u, 1000u), 0);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 6001u), 0);
    CHECK(dx_timer_wheel_next_timeout_ms(w, 1000000u) >= 0);

    dx_timer_wheel_destroy(w);
}

static void test_wheel_wraparound(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/wraparound");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);

    /* Deadline far in the future: the bucket index wraps around the ring. */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 300000u, 1000u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 301000u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 1000u), 300000);
    /* Not due yet. */
    CHECK_EQ(dx_timer_wheel_advance(w, 200000u, on_expire, &log), 0u);
    /* Due after the wrap. */
    CHECK_EQ(dx_timer_wheel_advance(w, 400000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 1u);
    CHECK_EQ(dx_timer_wheel_active(w), 0u);

    dx_timer_wheel_destroy(w);
}

static void test_guards(void)
{
    dx_timer_node_t a;
    dx_timer_wheel_t *w;

    dx_test_begin("timer/guards");
    dx_timer_node_reset(&a);

    w = dx_timer_wheel_create();
    CHECK(w != NULL);
    CHECK_EQ(dx_timer_wheel_start(NULL, &a, 100u, 0u), -1);
    CHECK_EQ(dx_timer_wheel_start(w, NULL, 100u, 0u), -1);
    CHECK_EQ(dx_timer_wheel_refresh(NULL, &a, 100u, 0u), -1);
    dx_timer_wheel_destroy(w);

    CHECK_EQ(dx_timer_wheel_advance(NULL, 0u, NULL, NULL), 0u);
    dx_timer_wheel_cancel(NULL, &a);
    dx_timer_node_reset(NULL);
    CHECK_EQ(dx_timer_node_deadline(NULL), 0u);
    CHECK_EQ(dx_timer_wheel_active(NULL), 0u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(NULL, 0u), (int)DX_TIMER_NO_TIMEOUT);
}

/*
 * Regression: a raise inside one bucket followed by a detach used to leave the
 * cached minimum in an empty bucket. next_timeout_ms() then answered 0 for
 * every clock, so the event loop spun on epoll_wait(0) forever.
 */
static void test_stale_slot_min_regression(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    fire_log_t log = {0, 0};

    dx_test_begin("timer/stale_slot_min");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);

    /* Lazy refresh into the same one-second bucket: slot_min keeps the old,
     * smaller deadline while node->deadline_ms moves past it. */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 60000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 59500u, 1000u), 0); /* deadline 60500 */
    CHECK_EQ(dx_timer_node_deadline(&a), 60500u);

    /* Moving to another bucket detaches with the raised deadline in hand: the
     * minimum of the old bucket must be recomputed, not assumed. */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 30000u, 2000u), 0);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 61000u), 0); /* a is still due */

    /* Dropping the survivor empties the wheel for good: the stale minimum of
     * the old bucket must not survive as a past deadline. */
    dx_timer_wheel_cancel(w, &a);
    CHECK_EQ(dx_timer_wheel_active(w), 0u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 61000u), (int)DX_TIMER_NO_TIMEOUT);

    /* Same shape through the fire path. */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 100000u), 0);
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 900u, 100100u), 0); /* deadline 101000 */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 30000u, 100200u), 0);
    CHECK_EQ(dx_timer_wheel_advance(w, 200000u, on_expire, &log), 1u);
    CHECK_EQ(log.fired, 1u);
    CHECK_EQ(dx_timer_wheel_active(w), 0u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 200000u), (int)DX_TIMER_NO_TIMEOUT);

    dx_timer_wheel_destroy(w);
}

/*
 * Regression: the fire loop must survive a callback that cancels or re-arms
 * nodes of the bucket it is walking.
 */
static void test_advance_with_mutation(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    dx_timer_node_t b;
    dx_timer_node_t c;

    dx_test_begin("timer/advance_mutation");
    if (w == NULL) {
        CHECK(0);
        return;
    }
    dx_timer_node_reset(&a);
    dx_timer_node_reset(&b);
    dx_timer_node_reset(&c);

    CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_start(w, &b, 1000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_start(w, &c, 1000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_active(w), 3u);

    /*
     * Three due nodes; the callback cancels b every time it runs. Exactly two
     * must fire and the wheel must end up empty. The old linear walk visited
     * the detached b, counted the active total down twice and refreshed the
     * cached minimum of a bucket it no longer belonged to.
     */
    {
        mutation_ctx_t ctx;

        ctx.wheel = w;
        ctx.victim = &b;
        ctx.fired = 0;
        CHECK_EQ(dx_timer_wheel_advance(w, 5000u, cancel_other_cb, &ctx), 2u);
        CHECK_EQ(ctx.fired, 2u);
    }
    CHECK_EQ(dx_timer_wheel_active(w), 0u);
    CHECK_EQ(dx_timer_node_deadline(&b), 0u);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 9000u), (int)DX_TIMER_NO_TIMEOUT);

    /* A callback that re-arms a node leaves it armed, and the loop waits for
     * the new deadline rather than the old one. */
    {
        rearm_ctx_t ctx;

        ctx.wheel = w;
        ctx.node = &a;
        ctx.now_ms = 15000u;
        ctx.count = 0;
        CHECK_EQ(dx_timer_wheel_start(w, &a, 1000u, 10000u), 0);
        CHECK_EQ(dx_timer_wheel_advance(w, 15000u, rearm_cb, &ctx), 1u);
        CHECK_EQ(ctx.count, 1u);
        CHECK_EQ(dx_timer_wheel_active(w), 1u);
        CHECK(dx_timer_node_deadline(&a) > 15000u);
        CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 15000u),
                 (int)(dx_timer_node_deadline(&a) - 15000u));
        CHECK_EQ(dx_timer_wheel_advance(w, 1000000u, on_expire_noop, &ctx), 1u);
    }
    CHECK_EQ(dx_timer_wheel_active(w), 0u);

    dx_timer_wheel_destroy(w);
}

/*
 * Regression: a lazy refresh that raises the deadline of the bucket's minimum
 * node used to leave the cached minimum pointing at the old, now past value.
 * next_timeout_ms() then answered 0 until the node was removed, spinning the
 * event loop on epoll_wait(0).
 */
static void test_lazy_refresh_raise(void)
{
    dx_timer_wheel_t *w = dx_timer_wheel_create();
    dx_timer_node_t a;
    uint64_t now;
    int false_wakeups = 0;

    dx_test_begin("timer/lazy_refresh_raise");
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    dx_timer_node_reset(&a);

    /* One node, its deadline raised 800 ms inside the same second bucket. */
    CHECK_EQ(dx_timer_wheel_start(w, &a, 10000u, 0u), 0);
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 10000u, 800u), 0);
    CHECK_EQ(dx_timer_node_deadline(&a), 10800u);

    /* The loop must wait 800 ms, not wake up on every clock. */
    for (now = 10000u; now < 10800u; now++) {
        if (dx_timer_wheel_next_timeout_ms(w, now) == 0) {
            false_wakeups++;
        }
    }
    CHECK_EQ(false_wakeups, 0);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 10000u), 800);
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 10700u), 100);

    /* Lowering the deadline in the same bucket still lowers the minimum. */
    CHECK_EQ(dx_timer_wheel_refresh(w, &a, 900u, 10000u), 0); /* deadline 10900 */
    CHECK_EQ(dx_timer_wheel_next_timeout_ms(w, 10000u), 900);

    dx_timer_wheel_destroy(w);
}

int main(void)
{
    test_lazy_refresh_raise();
    test_stale_slot_min_regression();
    test_advance_with_mutation();
    test_start_and_cancel();
    test_fires_at_deadline();
    test_same_bucket_not_fired_early();
    test_refresh();
    test_next_timeout();
    test_wheel_wraparound();
    test_guards();
    printf("test_timer: ");
    return dx_test_summary();
}
