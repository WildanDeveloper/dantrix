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
    uint8_t key_len;
    unsigned char key[DX_CL_KEY_MAX];
    /*
     * The publication flag, written LAST by the winner and read FIRST by a
     * lookup. It must be separate from the key bytes: a lookup that sees the
     * flag set is guaranteed to see the whole key, and until the flag is set the
     * slot is invisible.
     *
     * The claim order matters and was wrong twice before it was right:
     *   - writing the key and *then* checking whether the slot was free let a
     *     loser overwrite the winner's key, so two addresses shared a counter
     *     and one allowance doubled (found by the CI sanitizer job);
     *   - marking the slot claimed and *then* writing the key let a lookup see
     *     a claimed slot with an incomplete key, so that address was never
     *     found and its counter was lost (a lookup miss).
     * Only "write the key, then publish the flag" is correct.
     */
    _Atomic uint32_t state;
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
/*
 * Claim an empty slot for this key. Returns the slot on success, NULL when
 * another worker won the race for it.
 *
 * The order is: reserve the slot with a CAS, write the key, publish the flag.
 * The reservation CAS is what makes exactly one worker the winner, so only the
 * winner ever writes the key bytes -- a loser cannot clobber them. The flag is
 * published last with release ordering, so a lookup that observes it is
 * guaranteed to see the complete key.
 */
static dx_cl_slot_t *claim_slot(dx_cl_slot_t *s, const void *addr, size_t addr_len)
{
    uint32_t expected = 0u;

    if (!atomic_compare_exchange_strong_explicit(&s->state, &expected, 1u, memory_order_relaxed,
                                                 memory_order_relaxed)) {
        return NULL; /* somebody else claimed it */
    }
    /*
     * The slot is ours and invisible to lookups (state == 1 is not yet
     * published as a usable entry; see slot_matches). Fill it in, then publish.
     */
    memcpy(s->key, addr, addr_len);
    s->key_len = (uint8_t)addr_len;
    atomic_store_explicit(&s->count, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->state, 2u, memory_order_release); /* 2 = ready */
    return s;
}

/* True when this slot holds exactly this key. A slot is only comparable once
 * its owner has published it (state == 2). */
static bool slot_matches(const dx_cl_slot_t *s, const void *addr, size_t addr_len)
{
    if (atomic_load_explicit((_Atomic uint32_t *)&s->state, memory_order_acquire) != 2u) {
        return false;
    }
    return s->key_len == (uint8_t)addr_len && memcmp(s->key, addr, addr_len) == 0;
}

static dx_cl_slot_t *find_slot(dx_connlimit_t *cl, const void *addr, size_t addr_len,
                               bool create_slot)
{
    if (addr_len == 0u || addr_len > DX_CL_KEY_MAX) {
        return NULL;
    }
    for (uint32_t shard = 0; shard < DX_CL_SHARDS; shard++) {
        uint32_t h = key_hash(addr, addr_len, shard);
        dx_cl_slot_t *s = &cl->slots[(size_t)shard * cl->per_shard + (h % cl->per_shard)];

        if (slot_matches(s, addr, addr_len)) {
            return s;
        }
        if (create_slot && atomic_load_explicit(&s->state, memory_order_relaxed) == 0u) {
            dx_cl_slot_t *claimed = claim_slot(s, addr, addr_len);

            if (claimed != NULL) {
                return claimed;
            }
            /*
             * Lost the race: the winner may have claimed it for this very key,
             * so re-check before moving on to the next shard.
             */
            if (slot_matches(s, addr, addr_len)) {
                return s;
            }
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
    /*
     * A read-only lookup, written out here rather than reusing find_slot() with
     * a cast that drops const: that cast would let a future edit write through
     * a const handle, and it hid the fact that counting never claims a slot.
     */
    if (cl == NULL || addr_len == 0u || addr_len > DX_CL_KEY_MAX) {
        return 0u;
    }
    for (uint32_t shard = 0; shard < DX_CL_SHARDS; shard++) {
        uint32_t h = key_hash(addr, addr_len, shard);
        const dx_cl_slot_t *s = &cl->slots[(size_t)shard * cl->per_shard + (h % cl->per_shard)];

        if (slot_matches(s, addr, addr_len)) {
            return atomic_load_explicit(&s->count, memory_order_relaxed);
        }
    }
    return 0u;
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
        /* A slot counts as tracked once it is published, even if its count has
         * since returned to zero (the entry stays until it is reused). */
        if (atomic_load_explicit((_Atomic uint32_t *)&cl->slots[i].state, memory_order_relaxed) ==
            2u) {
            n++;
        }
    }
    return n;
}

uint64_t dx_connlimit_refused(const dx_connlimit_t *cl)
{
    return (cl != NULL) ? atomic_load_explicit(&cl->refused, memory_order_relaxed) : 0u;
}
