#include "connlimit.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * Shard count. A key hashes into one slot in every shard, and the slot that
 * matches the key is used: with 16 shards a collision needs two distinct
 * addresses to agree modulo every shard, which is rare for the small number of
 * addresses that hold connections at once. The value is a power of two so the
 * shard index is a mask, not a division.
 */
#define DX_CL_SHARDS 16u

/* Largest address we can key on. 16 bytes covers IPv6 and IPv4-mapped v6. */
#define DX_CL_KEY_MAX 16u

typedef struct {
    _Atomic uint64_t count;
    unsigned char key[DX_CL_KEY_MAX];
    uint8_t key_len;
} dx_cl_slot_t;

struct dx_connlimit {
    dx_cl_slot_t *slots;
    size_t cap; /* total slots, a multiple of DX_CL_SHARDS */
    size_t per_shard;
    _Atomic uint64_t refused;
};

/*
 * FNV-1a over the address bytes plus the shard index, so each shard uses a
 * different starting slot for the same key. Mixing the shard in is what makes a
 * collision require agreement in every shard rather than in one.
 */
static uint32_t key_hash(const void *addr, size_t len, uint32_t shard)
{
    const unsigned char *p = (const unsigned char *)addr;
    uint32_t h = 2166136261u ^ (shard * 2654435761u);

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

dx_connlimit_t *dx_connlimit_create(size_t slots)
{
    dx_connlimit_t *cl;
    size_t rounded;

    if (slots == 0u) {
        return NULL;
    }
    /* Round up to a whole number of shards, at least one slot per shard. */
    rounded = ((slots + DX_CL_SHARDS - 1u) / DX_CL_SHARDS) * DX_CL_SHARDS;
    if (rounded < DX_CL_SHARDS) {
        rounded = DX_CL_SHARDS;
    }

    cl = (dx_connlimit_t *)calloc(1, sizeof(*cl));
    if (cl == NULL) {
        return NULL;
    }
    cl->slots = (dx_cl_slot_t *)calloc(rounded, sizeof(dx_cl_slot_t));
    if (cl->slots == NULL) {
        free(cl);
        return NULL;
    }
    cl->cap = rounded;
    cl->per_shard = rounded / DX_CL_SHARDS;
    return cl;
}

void dx_connlimit_destroy(dx_connlimit_t *cl)
{
    if (cl == NULL) {
        return;
    }
    free(cl->slots);
    free(cl);
}

/*
 * Find the slot for this key, or NULL when the key is not tracked and there is
 * no free slot in any shard (a full table). When create_slot is true a free
 * slot is claimed; otherwise only an existing entry is returned.
 */
static dx_cl_slot_t *find_slot(dx_connlimit_t *cl, const void *addr, size_t addr_len,
                               bool create_slot)
{
    if (addr_len == 0u || addr_len > DX_CL_KEY_MAX) {
        return NULL;
    }
    for (uint32_t shard = 0; shard < DX_CL_SHARDS; shard++) {
        uint32_t h = key_hash(addr, addr_len, shard);
        dx_cl_slot_t *s = &cl->slots[(size_t)shard * cl->per_shard + (h % cl->per_shard)];
        uint8_t len = atomic_load_explicit((_Atomic uint8_t *)&s->key_len, memory_order_acquire);

        if (len == (uint8_t)addr_len && memcmp(s->key, addr, addr_len) == 0) {
            return s;
        }
        if (create_slot && len == 0u) {
            /*
             * Claim the slot. The length is published last with release
             * ordering, so another worker that sees a non-zero length is
             * guaranteed to see the key bytes that go with it.
             */
            memcpy(s->key, addr, addr_len);
            atomic_store_explicit(&s->count, 0u, memory_order_relaxed);
            atomic_store_explicit((_Atomic uint8_t *)&s->key_len, (uint8_t)addr_len,
                                  memory_order_release);
            return s;
        }
    }
    return NULL;
}

bool dx_connlimit_acquire(dx_connlimit_t *cl, const void *addr, size_t addr_len, unsigned limit)
{
    dx_cl_slot_t *s;
    uint64_t seen;

    if (cl == NULL || limit == 0u) {
        return true; /* disabled or unlimited */
    }
    s = find_slot(cl, addr, addr_len, true);
    if (s == NULL) {
        /* Table full or an unusable key: fail open rather than deny service to
         * a client that has done nothing wrong. */
        return true;
    }
    /*
     * Reserve with a compare-and-swap loop so two workers cannot both slip past
     * the limit. The count is only incremented when it is below the limit.
     */
    seen = atomic_load_explicit(&s->count, memory_order_relaxed);
    for (;;) {
        if (seen >= (uint64_t)limit) {
            atomic_fetch_add_explicit(&cl->refused, 1u, memory_order_relaxed);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&s->count, &seen, seen + 1u, memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            return true;
        }
        /* seen was refreshed by the failed exchange: retry with the new value. */
    }
}

void dx_connlimit_release(dx_connlimit_t *cl, const void *addr, size_t addr_len)
{
    dx_cl_slot_t *s;

    if (cl == NULL) {
        return;
    }
    s = find_slot(cl, addr, addr_len, false);
    if (s == NULL) {
        return;
    }
    /*
     * Decrement without going below zero. A stray release (a connection closed
     * on a path that never acquired) must not make the counter wrap to a huge
     * value, which would then deny that address forever.
     */
    {
        uint64_t seen = atomic_load_explicit(&s->count, memory_order_relaxed);

        while (seen > 0u) {
            if (atomic_compare_exchange_weak_explicit(&s->count, &seen, seen - 1u,
                                                      memory_order_acq_rel, memory_order_relaxed)) {
                return;
            }
        }
    }
}

uint64_t dx_connlimit_count(const dx_connlimit_t *cl, const void *addr, size_t addr_len)
{
    /* find_slot() takes a non-const table; this cast only reads. */
    dx_cl_slot_t *s = find_slot((dx_connlimit_t *)cl, addr, addr_len, false);

    return (s != NULL) ? atomic_load_explicit(&s->count, memory_order_relaxed) : 0u;
}

uint64_t dx_connlimit_total(const dx_connlimit_t *cl)
{
    uint64_t total = 0;

    if (cl == NULL) {
        return 0;
    }
    for (size_t i = 0; i < cl->cap; i++) {
        total += atomic_load_explicit(&cl->slots[i].count, memory_order_relaxed);
    }
    return total;
}

uint64_t dx_connlimit_distinct(const dx_connlimit_t *cl)
{
    uint64_t n = 0;

    if (cl == NULL) {
        return 0;
    }
    for (size_t i = 0; i < cl->cap; i++) {
        if (atomic_load_explicit(&cl->slots[i].count, memory_order_relaxed) > 0u) {
            n++;
        }
    }
    return n;
}

uint64_t dx_connlimit_refused(const dx_connlimit_t *cl)
{
    return (cl != NULL) ? atomic_load_explicit(&cl->refused, memory_order_relaxed) : 0u;
}
