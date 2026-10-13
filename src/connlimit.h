/*
 * connlimit.h - Per-IP connection accounting.
 *
 * Purpose: stop one client from taking every connection slot. Without it, a
 * single host opening max_connections sockets denies service to everyone else,
 * which is the cheapest denial-of-service there is.
 *
 * Design, chosen to keep the accept path cheap and lock-free:
 *
 *   * Sharded, direct-mapped counters. The table is split into N shards and a
 *     key (the client address) hashes into exactly one slot per shard. There is
 *     no probe chain and no rehash: a collision means the entry is not tracked,
 *     which fails *open* (the connection is allowed) rather than closing the
 *     wrong client's connections.
 *   * Atomic slots. Each slot is a key plus an _Atomic count, so a worker
 *     increments without a lock. Two workers that hash to the same slot with
 *     different keys will see a mismatch and both give up tracking, which is
 *     the documented collision behaviour, not a bug.
 *   * Exact when it matters. With the default shard count and a per-IP limit of
 *     a few hundred, the table tracks tens of thousands of distinct addresses
 *     before collisions appear; a flood is exactly the case where one address
 *     dominates and the slot is never contended.
 *
 * The limit is checked before the connection is registered and released when it
 * closes, so the count tracks *live* connections, not connection attempts.
 */
#ifndef DX_CONNLIMIT_H
#define DX_CONNLIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct dx_connlimit dx_connlimit_t;

/*
 * Create the table. slots is rounded up to a multiple of the shard count and
 * must be non-zero; a zero limit_ms or slots == 0 disables accounting and makes
 * every function a no-op that allows the connection (so the caller can pass the
 * handle around unconditionally).
 */
dx_connlimit_t *dx_connlimit_create(size_t slots);

void dx_connlimit_destroy(dx_connlimit_t *cl);

/*
 * Try to reserve one connection for the client address.
 *
 * Returns true when the connection may proceed. When the address already holds
 * `limit` live connections the call returns false and reserves nothing; the
 * caller then closes the socket.
 *
 * limit == 0 means unlimited and always returns true. A NULL table always
 * returns true.
 */
bool dx_connlimit_acquire(dx_connlimit_t *cl, const void *addr, size_t addr_len, unsigned limit);

/*
 * Release one connection for the client address. Safe to call for an address
 * that was never acquired: it is a no-op then. Must be called exactly once per
 * successful acquire.
 */
void dx_connlimit_release(dx_connlimit_t *cl, const void *addr, size_t addr_len);

/* Live connections currently tracked for this address. */
uint64_t dx_connlimit_count(const dx_connlimit_t *cl, const void *addr, size_t addr_len);

/* Total live connections across every tracked address. */
uint64_t dx_connlimit_total(const dx_connlimit_t *cl);

/* Number of addresses with a live count above zero. */
uint64_t dx_connlimit_distinct(const dx_connlimit_t *cl);

/* How many acquire attempts were refused since start-up. */
uint64_t dx_connlimit_refused(const dx_connlimit_t *cl);

#endif /* DX_CONNLIMIT_H */
