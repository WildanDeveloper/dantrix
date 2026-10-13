/*
 * fcache.h - Per-worker open-file cache.
 *
 * Serving a static file costs an openat2() plus an fstat() on every request.
 * For a workload that repeats the same few files (a home page, a CSS file, a
 * favicon) that is pure overhead: the file has not changed, only the socket
 * has. The cache keeps a small set of descriptors open and hands them back for
 * the validity window, so a repeated request costs no open and no stat.
 *
 * Design constraints, chosen for safety over cleverness:
 *
 *   * One cache per worker. There is no lock anywhere: the worker's event loop
 *     is the only thread that touches it, exactly like the connection pool.
 *   * Direct mapped. A path hashes to one slot; there is no probe chain and
 *     therefore no unbounded lookup. A collision simply does not cache.
 *   * Explicit pinning. A descriptor handed to a connection is pinned until
 *     that connection stops using it. A pinned slot is never replaced, so the
 *     cache can never close a descriptor out from under a live transfer.
 *   * Bounded validity. An entry older than valid_ms is re-opened, so a file
 *     replaced on disk is picked up within the window. This is the same
 *     trade-off every server-side open-file cache makes.
 *
 * The fd is opened O_RDONLY, so sharing it between connections is safe as long
 * as every transfer uses an explicit offset (sendfile with &offset, pread),
 * which the serving path already does.
 */
#ifndef DX_FCACHE_H
#define DX_FCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

/* Longest relative path the cache will remember. Longer paths are not cached. */
#define DX_FCACHE_PATH_MAX 512u

/* One slot. Callers treat it as opaque except for releasing it. */
typedef struct dx_fcache_entry {
    char path[DX_FCACHE_PATH_MAX];
    struct stat st;
    int fd;              /* -1 when the slot is empty */
    uint64_t expires_ms; /* monotonic time after which the entry is stale */
    unsigned refs;       /* connections currently streaming this descriptor */
} dx_fcache_entry_t;

typedef struct dx_fcache dx_fcache_t;

/*
 * Create a cache with the given number of slots and validity window (in
 * milliseconds). slots == 0 or valid_ms == 0 disables caching entirely and
 * returns NULL, which every other function accepts as "no cache".
 */
dx_fcache_t *dx_fcache_create(size_t slots, uint64_t valid_ms);

/* Close every cached descriptor and release the table. */
void dx_fcache_destroy(dx_fcache_t *c);

/*
 * Look up path. On a hit the entry is pinned (refs++) and its stat is copied
 * into *st; the caller must release it with dx_fcache_release() when done. On a
 * miss (or when c is NULL) returns NULL and leaves *st untouched.
 */
dx_fcache_entry_t *dx_fcache_get(dx_fcache_t *c, const char *path, uint64_t now_ms,
                                 struct stat *st);

/*
 * Offer a freshly opened descriptor to the cache. On success the cache owns fd
 * and pins the entry for the caller, which receives it through *out_entry (and
 * must release it). Returns false when the path is too long, the slot is pinned
 * by another transfer, or c is NULL: the caller then keeps ownership of fd.
 */
bool dx_fcache_put(dx_fcache_t *c, const char *path, int fd, const struct stat *st, uint64_t now_ms,
                   dx_fcache_entry_t **out_entry);

/*
 * Unpin an entry returned by dx_fcache_get() or dx_fcache_put(). Safe to call
 * with NULL. The descriptor stays cached and open; it is only released when the
 * slot is evicted by a later insert.
 */
void dx_fcache_release(dx_fcache_entry_t *e);

/* Number of live entries (for the status endpoint and tests). */
size_t dx_fcache_count(const dx_fcache_t *c);

#endif /* DX_FCACHE_H */
