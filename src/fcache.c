#include "fcache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dantrix.h"
#include "fdcount.h"

struct dx_fcache {
    dx_fcache_entry_t *entries;
    size_t cap;
    uint64_t valid_ms;
};

/*
 * FNV-1a over the path bytes. It spreads the short relative paths of a document
 * root well and needs no seed: every worker builds the same table for the same
 * file set, which keeps behaviour reproducible between runs.
 */
static uint32_t path_hash(const char *p)
{
    uint32_t h = 2166136261u;

    while (*p != '\0') {
        h ^= (unsigned char)*p++;
        h *= 16777619u;
    }
    return h;
}

/* True when the slot holds this exact path with a still-valid entry. */
static bool slot_is_fresh_hit(const dx_fcache_entry_t *e, const char *path, size_t len,
                              uint64_t now_ms)
{
    return e->fd >= 0 && strlen(e->path) == len && memcmp(e->path, path, len) == 0 &&
           now_ms < e->expires_ms;
}

dx_fcache_t *dx_fcache_create(size_t slots, uint64_t valid_ms)
{
    dx_fcache_t *c;

    /* A zero slot count or a zero window means "do not cache". */
    if (slots == 0u || valid_ms == 0u) {
        return NULL;
    }
    c = (dx_fcache_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->entries = (dx_fcache_entry_t *)calloc(slots, sizeof(dx_fcache_entry_t));
    if (c->entries == NULL) {
        free(c);
        return NULL;
    }
    c->cap = slots;
    c->valid_ms = valid_ms;
    for (size_t i = 0; i < slots; i++) {
        c->entries[i].fd = -1;
    }
    return c;
}

void dx_fcache_destroy(dx_fcache_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->entries != NULL) {
        for (size_t i = 0; i < c->cap; i++) {
            /* An entry that is still pinned must not be closed: the owning
             * connection is still streaming it. destroy() only runs at worker
             * shutdown, after every connection has been closed, but the check
             * costs nothing and makes the invariant explicit. */
            if (c->entries[i].fd >= 0 && c->entries[i].refs == 0u) {
                (void)close(c->entries[i].fd);
                dx_fdcount_closed(DX_FD_CACHE);
            }
        }
        free(c->entries);
        c->entries = NULL;
    }
    free(c);
}

dx_fcache_entry_t *dx_fcache_get(dx_fcache_t *c, const char *path, uint64_t now_ms, struct stat *st)
{
    dx_fcache_entry_t *e;
    size_t len;

    if (c == NULL || path == NULL) {
        return NULL;
    }
    len = strlen(path);
    if (len == 0u || len >= DX_FCACHE_PATH_MAX) {
        return NULL;
    }
    e = &c->entries[path_hash(path) % (uint32_t)c->cap];

    if (e->fd < 0 || strlen(e->path) != len || memcmp(e->path, path, len) != 0) {
        return NULL; /* empty or a different file in this slot */
    }
    if (now_ms >= e->expires_ms) {
        /*
         * Stale: the file may have been replaced on disk. A pinned entry is
         * still being streamed by another connection, so it must keep its
         * descriptor until that transfer finishes; it is not handed out again.
         */
        if (e->refs == 0u) {
            (void)close(e->fd);
            dx_fdcount_closed(DX_FD_CACHE);
            e->fd = -1;
        }
        return NULL;
    }
    /*
     * Re-stat instead of trusting the size captured when the entry was
     * inserted. A file truncated in place keeps the same inode, so the cached
     * st_size stops matching the bytes the descriptor will return: the server
     * then advertises Content-Length: 1000 and sends 10, leaving the keep-alive
     * stream 990 bytes short -- the next response is written into the middle of
     * this body (a framing primitive a proxy can be tricked by). Reproduced
     * against v1.0.3: second request on the same connection timed out after a
     * truncated body. fstat() is cheap; the cache exists to skip openat2(), not
     * to skip a stat.
     */
    if (fstat(e->fd, st) != 0) {
        return NULL;
    }
    e->st = *st;
    e->refs++;
    return e;
}

bool dx_fcache_put(dx_fcache_t *c, const char *path, int fd, const struct stat *st, uint64_t now_ms,
                   dx_fcache_entry_t **out_entry)
{
    dx_fcache_entry_t *e;
    size_t len;

    if (c == NULL || out_entry == NULL || fd < 0) {
        return false;
    }
    len = strlen(path);
    if (len == 0u || len >= DX_FCACHE_PATH_MAX) {
        return false;
    }
    e = &c->entries[path_hash(path) % (uint32_t)c->cap];

    /*
     * A slot that already holds this very file with a valid entry is simply
     * kept: the caller's freshly opened descriptor is redundant and is closed
     * right away. That is a cache hit, not a miss.
     */
    if (slot_is_fresh_hit(e, path, len, now_ms)) {
        (void)close(fd);
        dx_fdcount_closed(DX_FD_FILE); /* the caller opened it; it is redundant */
        e->refs++;
        *out_entry = e;
        return true;
    }
    /*
     * The slot is empty, stale, or holds a different path. Replacing it closes
     * its descriptor, which must only happen when nothing is streaming it: a
     * pinned entry is left exactly as it is and the caller keeps its own fd.
     */
    if (e->fd >= 0) {
        if (e->refs > 0u) {
            return false;
        }
        (void)close(e->fd);
        dx_fdcount_closed(DX_FD_CACHE);
    }
    /* Ownership of the caller's descriptor (counted as a served file) moves to
     * the cache: reclassify it so the believed total stays correct. */
    dx_fdcount_closed(DX_FD_FILE);
    dx_fdcount_opened(DX_FD_CACHE);
    e->fd = fd;
    e->st = *st;
    e->expires_ms = now_ms + c->valid_ms;
    e->refs = 1u;
    memcpy(e->path, path, len + 1u);
    *out_entry = e;
    return true;
}

void dx_fcache_release(dx_fcache_entry_t *e)
{
    if (e != NULL && e->refs > 0u) {
        e->refs--;
    }
}

size_t dx_fcache_count(const dx_fcache_t *c)
{
    size_t n = 0;

    if (c == NULL) {
        return 0;
    }
    for (size_t i = 0; i < c->cap; i++) {
        if (c->entries[i].fd >= 0) {
            n++;
        }
    }
    return n;
}
