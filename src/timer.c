#include "timer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dx_timer_wheel {
    dx_timer_node_t *slots[DX_TIMER_SLOTS];
    /* Earliest deadline per bucket; UINT64_MAX when the bucket is empty. Keeps
     * the epoll timeout computation independent of the list lengths. */
    uint64_t slot_min[DX_TIMER_SLOTS];
    uint64_t current_ms;   /* clock value of the last advance() */
    uint64_t processed_ms; /* end of the last fully inspected tick */
    size_t active;
};

dx_timer_wheel_t *dx_timer_wheel_create(void)
{
    dx_timer_wheel_t *wheel = (dx_timer_wheel_t *)calloc(1, sizeof(*wheel));

    if (wheel == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < DX_TIMER_SLOTS; i++) {
        wheel->slot_min[i] = UINT64_MAX;
    }
    return wheel;
}

void dx_timer_wheel_destroy(dx_timer_wheel_t *wheel)
{
    free(wheel);
}

void dx_timer_node_reset(dx_timer_node_t *node)
{
    if (node == NULL) {
        return;
    }
    node->prev = NULL;
    node->next = NULL;
    node->owner = NULL;
    node->slot = 0;
    node->deadline_ms = 0;
    node->active = false;
}

uint64_t dx_timer_node_deadline(const dx_timer_node_t *node)
{
    return (node != NULL && node->active) ? node->deadline_ms : 0;
}

size_t dx_timer_wheel_active(const dx_timer_wheel_t *wheel)
{
    return (wheel == NULL) ? 0u : wheel->active;
}

/* Bucket for an absolute deadline: one bucket per second. */
static uint32_t bucket_of(uint64_t deadline_ms)
{
    return (uint32_t)((deadline_ms / DX_TIMER_TICK_MS) % DX_TIMER_SLOTS);
}

/* Recompute the cached minimum of a bucket after a removal. */
static void refresh_slot_min(dx_timer_wheel_t *wheel, uint32_t slot)
{
    const dx_timer_node_t *node;
    uint64_t best = UINT64_MAX;

    for (node = wheel->slots[slot]; node != NULL; node = node->next) {
        if (node->deadline_ms < best) {
            best = node->deadline_ms;
        }
    }
    wheel->slot_min[slot] = best;
}

/* Unlink a node from its bucket. The caller must know it is active. */
static void detach(dx_timer_wheel_t *wheel, dx_timer_node_t *node)
{
    uint32_t slot = node->slot;

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        wheel->slots[slot] = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    node->prev = NULL;
    node->next = NULL;
    node->active = false;
    if (wheel->active > 0) {
        wheel->active--;
    }
    if (node->deadline_ms == wheel->slot_min[slot]) {
        refresh_slot_min(wheel, slot);
    }
}

/* Link a node into the bucket its deadline belongs to. */
static void attach(dx_timer_wheel_t *wheel, dx_timer_node_t *node, uint64_t deadline_ms)
{
    uint32_t slot = bucket_of(deadline_ms);

    node->slot = slot;
    node->deadline_ms = deadline_ms;
    node->prev = NULL;
    node->next = wheel->slots[slot];
    if (wheel->slots[slot] != NULL) {
        wheel->slots[slot]->prev = node;
    }
    wheel->slots[slot] = node;
    node->active = true;
    wheel->active++;
    if (deadline_ms < wheel->slot_min[slot]) {
        wheel->slot_min[slot] = deadline_ms;
    }
}

void dx_timer_wheel_cancel(dx_timer_wheel_t *wheel, dx_timer_node_t *node)
{
    if (wheel == NULL || node == NULL || !node->active) {
        return;
    }
    detach(wheel, node);
    node->deadline_ms = 0;
}

int dx_timer_wheel_start(dx_timer_wheel_t *wheel, dx_timer_node_t *node, uint64_t delay_ms,
                         uint64_t now_ms)
{
    if (wheel == NULL || node == NULL) {
        return -1;
    }
    dx_timer_wheel_cancel(wheel, node); /* re-arming replaces the old deadline */
    attach(wheel, node, now_ms + delay_ms);
    return 0;
}

int dx_timer_wheel_refresh(dx_timer_wheel_t *wheel, dx_timer_node_t *node, uint64_t delay_ms,
                           uint64_t now_ms)
{
    uint64_t deadline;

    if (wheel == NULL || node == NULL) {
        return -1;
    }
    if (!node->active) {
        return dx_timer_wheel_start(wheel, node, delay_ms, now_ms);
    }
    deadline = now_ms + delay_ms;
    if (deadline == node->deadline_ms) {
        return 0;
    }
    /*
     * Lazy refresh: a move inside the same second keeps the node where it is
     * and only records the new deadline.
     */
    if (bucket_of(deadline) == node->slot) {
        node->deadline_ms = deadline;
        if (deadline < wheel->slot_min[node->slot]) {
            wheel->slot_min[node->slot] = deadline;
        }
        return 0;
    }
    detach(wheel, node);
    attach(wheel, node, deadline);
    return 0;
}

int dx_timer_wheel_next_timeout_ms(const dx_timer_wheel_t *wheel, uint64_t now_ms)
{
    uint64_t best = UINT64_MAX;

    if (wheel == NULL) {
        return (int)DX_TIMER_NO_TIMEOUT;
    }
    for (size_t i = 0; i < DX_TIMER_SLOTS; i++) {
        if (wheel->slot_min[i] < best) {
            best = wheel->slot_min[i];
        }
    }
    if (best == UINT64_MAX) {
        return (int)DX_TIMER_NO_TIMEOUT;
    }
    if (best <= now_ms) {
        return 0;
    }
    if (best - now_ms > (uint64_t)DX_TIMER_NO_TIMEOUT) {
        return (int)DX_TIMER_NO_TIMEOUT;
    }
    return (int)(best - now_ms);
}

size_t dx_timer_wheel_advance(dx_timer_wheel_t *wheel, uint64_t now_ms,
                              dx_timer_expire_fn on_expire, void *ctx)
{
    uint64_t start_tick;
    uint64_t end_tick;
    size_t steps;
    size_t fired = 0;

    if (wheel == NULL) {
        return 0;
    }
    if (now_ms > wheel->current_ms) {
        wheel->current_ms = now_ms;
    }
    if (now_ms < wheel->processed_ms) {
        /* The clock moved backwards (never observed, but cheap to survive). */
        wheel->processed_ms = now_ms;
    }

    start_tick = wheel->processed_ms / DX_TIMER_TICK_MS;
    end_tick = now_ms / DX_TIMER_TICK_MS;
    steps = (end_tick >= start_tick) ? (size_t)(end_tick - start_tick + 1u) : 1u;
    if (steps > DX_TIMER_SLOTS) {
        steps = DX_TIMER_SLOTS; /* one full turn is always enough */
    }

    for (size_t s = 0; s < steps; s++) {
        uint32_t bucket = (uint32_t)((start_tick + s) % DX_TIMER_SLOTS);
        dx_timer_node_t *node = wheel->slots[bucket];

        while (node != NULL) {
            dx_timer_node_t *next = node->next;

            /*
             * Fire against the absolute deadline, not the bucket turn, so a
             * timer that shares a bucket with a later one is not fired early.
             */
            if (node->deadline_ms <= now_ms) {
                detach(wheel, node);
                fired++;
                if (on_expire != NULL) {
                    on_expire(ctx, node);
                }
            }
            node = next;
        }
    }
    wheel->processed_ms = end_tick * DX_TIMER_TICK_MS;
    return fired;
}
