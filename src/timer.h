/*
 * timer.h - Hashed timing wheel with one intrusive node per connection.
 *
 * Shape from the README: 256 buckets of one second, so installing, refreshing
 * and cancelling are O(1) and no allocation ever happens. Deadlines are kept
 * as absolute monotonic milliseconds, which is a small simplification of the
 * classic rotation-counter scheme: a timer fires exactly when its deadline is
 * reached (checked against the clock, not against the bucket turn), and a
 * timer that moves by less than one second keeps its bucket, which is the
 * "lazy refresh" that keeps a busy connection from churning the list on every
 * event.
 *
 * The wheel never sleeps by itself: the event loop asks for the millisecond
 * timeout of the nearest deadline, passes that to epoll_wait() and then calls
 * dx_timer_wheel_advance() to fire whatever came due.
 */
#ifndef DX_TIMER_H
#define DX_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DX_TIMER_SLOTS 256u
#define DX_TIMER_TICK_MS 1000u

/* One node per connection, embedded in dx_conn_t and never allocated. */
typedef struct dx_timer_node {
    struct dx_timer_node *prev;
    struct dx_timer_node *next;
    void *owner;          /* the connection; NULL while unarmed */
    uint32_t slot;        /* bucket index, valid while active */
    uint64_t deadline_ms; /* absolute, monotonic milliseconds */
    bool active;
} dx_timer_node_t;

typedef struct dx_timer_wheel dx_timer_wheel_t;

/* Callback for an expired timer. The node is already detached. */
typedef void (*dx_timer_expire_fn)(void *ctx, dx_timer_node_t *node);

/* Deadlines use dx_now_ms() from util.h as their clock. */

dx_timer_wheel_t *dx_timer_wheel_create(void);
void dx_timer_wheel_destroy(dx_timer_wheel_t *wheel);

/* Clear a node so it can be reused (call after the owner is gone). */
void dx_timer_node_reset(dx_timer_node_t *node);

/*
 * Install (or re-install) a timer that fires delay_ms from now.
 * Returns 0 on success, -1 on invalid arguments.
 */
int dx_timer_wheel_start(dx_timer_wheel_t *wheel, dx_timer_node_t *node, uint64_t delay_ms,
                         uint64_t now_ms);

/*
 * Move an active timer to delay_ms from now. The node only changes bucket
 * when the deadline moves by at least one tick.
 */
int dx_timer_wheel_refresh(dx_timer_wheel_t *wheel, dx_timer_node_t *node, uint64_t delay_ms,
                           uint64_t now_ms);

/* Detach an active timer. Safe on an inactive node. */
void dx_timer_wheel_cancel(dx_timer_wheel_t *wheel, dx_timer_node_t *node);

/*
 * Milliseconds until the nearest deadline, 0 when a timer is already due and
 * DX_TIMER_NO_TIMEOUT when the wheel is empty. Never returns a negative value.
 */
#define DX_TIMER_NO_TIMEOUT 3600000u
int dx_timer_wheel_next_timeout_ms(const dx_timer_wheel_t *wheel, uint64_t now_ms);

/*
 * Fire every timer whose deadline is at or before now_ms and invoke the
 * callback for each. Returns the number of timers fired.
 */
size_t dx_timer_wheel_advance(dx_timer_wheel_t *wheel, uint64_t now_ms,
                              dx_timer_expire_fn on_expire, void *ctx);

/* Number of timers currently armed (diagnostics and tests). */
size_t dx_timer_wheel_active(const dx_timer_wheel_t *wheel);

/* Absolute deadline of a node, or 0 when it is not armed. */
uint64_t dx_timer_node_deadline(const dx_timer_node_t *node);

#endif /* DX_TIMER_H */
