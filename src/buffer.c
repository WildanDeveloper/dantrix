#include "buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A chunk is one allocation holding chunk_slots equally sized buffers plus a
 * bitmap that records which slots are currently owned by a caller.
 */
typedef struct dx_chunk {
    struct dx_chunk *next;
    unsigned char *mem;
    unsigned char *in_use;
    size_t slot_size;
    size_t slots;
    size_t bits;
} dx_chunk_t;

/*
 * A free slot is addressed by a single 64 bit number: the chunk ordinal in the
 * high 32 bits and the slot ordinal in the low 32 bits. Keeping free slots in
 * one growable array makes acquire O(1) and keeps the chunks themselves free
 * of any bookkeeping structure.
 */
struct dx_bufpool {
    dx_chunk_t *chunks;
    uint64_t *free_list;
    size_t free_count;
    size_t free_cap;
    size_t slot_size;
    size_t chunk_slots;
    size_t total_slots;
    size_t outstanding;
};

void dx_buffer_init(dx_buffer_t *b, unsigned char *data, size_t cap)
{
    if (b == NULL) {
        return;
    }
    b->data = data;
    b->cap = cap;
    b->off = 0;
    b->len = 0;
}

void dx_buffer_detach(dx_buffer_t *b)
{
    if (b == NULL) {
        return;
    }
    b->data = NULL;
    b->cap = 0;
    b->off = 0;
    b->len = 0;
}

size_t dx_buffer_readable(const dx_buffer_t *b)
{
    if (b == NULL || b->len <= b->off) {
        return 0;
    }
    return b->len - b->off;
}

size_t dx_buffer_writable(const dx_buffer_t *b)
{
    if (b == NULL || b->cap <= b->len) {
        return 0;
    }
    return b->cap - b->len;
}

void dx_buffer_consume(dx_buffer_t *b, size_t n)
{
    if (b == NULL) {
        return;
    }
    size_t avail = dx_buffer_readable(b);

    if (n > avail) {
        n = avail;
    }
    b->off += n;
    if (b->off >= b->len) {
        /* Fully drained: reset so the window starts at the beginning again. */
        b->off = 0;
        b->len = 0;
    }
}

void dx_buffer_compact(dx_buffer_t *b)
{
    size_t live;

    if (b == NULL || b->off == 0) {
        return;
    }
    live = dx_buffer_readable(b);
    if (live > 0) {
        memmove(b->data, b->data + b->off, live);
    }
    b->off = 0;
    b->len = live;
}

bool dx_buffer_append(dx_buffer_t *b, const void *src, size_t n)
{
    if (b == NULL || b->data == NULL) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    if (src == NULL) {
        return false;
    }

    dx_buffer_compact(b);
    if (n > dx_buffer_writable(b)) {
        return false;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

/* Bitmap helpers. Slot ordinals are dense and bounded by the chunk size. */
static bool bit_test(const unsigned char *map, size_t i)
{
    return (map[i >> 3] & (unsigned char)(1u << (i & 7u))) != 0;
}

static void bit_set(unsigned char *map, size_t i)
{
    map[i >> 3] |= (unsigned char)(1u << (i & 7u));
}

static void bit_clear(unsigned char *map, size_t i)
{
    map[i >> 3] &= (unsigned char)~(1u << (i & 7u));
}

/* Append one free slot address to the free list, growing it if needed. */
static bool free_list_push(dx_bufpool_t *pool, uint32_t chunk_index, uint32_t slot_index)
{
    if (pool->free_count == pool->free_cap) {
        size_t new_cap = (pool->free_cap == 0) ? 64u : pool->free_cap * 2u;
        uint64_t *p;

        if (new_cap < pool->free_cap) {
            return false; /* overflow of size_t */
        }
        p = (uint64_t *)realloc(pool->free_list, new_cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        pool->free_list = p;
        pool->free_cap = new_cap;
    }
    pool->free_list[pool->free_count++] =
        ((uint64_t)chunk_index << 32) | (uint64_t)slot_index;
    return true;
}

/* Reserve room for n more entries; false when memory runs out. */
static bool free_list_reserve(dx_bufpool_t *pool, size_t n)
{
    size_t needed = pool->free_count + n;
    size_t new_cap;

    if (needed <= pool->free_cap) {
        return true;
    }
    new_cap = (pool->free_cap == 0) ? 64u : pool->free_cap;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2u) {
            return false; /* doubling would overflow */
        }
        new_cap *= 2u;
    }
    {
        uint64_t *p = (uint64_t *)realloc(pool->free_list, new_cap * sizeof(*p));

        if (p == NULL) {
            return false;
        }
        pool->free_list = p;
        pool->free_cap = new_cap;
    }
    return true;
}

/*
 * Allocate one more chunk and register all of its slots as free. Returns the
 * ordinal of the new chunk, or SIZE_MAX on failure.
 *
 * Chunks are appended at the tail, never prepended: the free list encodes the
 * chunk ordinal, so the head must stay at ordinal 0 for the whole lifetime of
 * the pool.
 */
static size_t bufpool_add_chunk(dx_bufpool_t *pool)
{
    dx_chunk_t *chunk;
    dx_chunk_t **tail;
    size_t bytes;
    size_t index = 0;

    for (tail = &pool->chunks; *tail != NULL; tail = &(*tail)->next) {
        index++;
    }

    bytes = pool->slot_size * pool->chunk_slots;
    if (pool->slot_size != 0 && bytes / pool->slot_size != pool->chunk_slots) {
        return (size_t)-1; /* size overflow */
    }

    /*
     * Grow the free list first: if this fails, bail out before any chunk
     * memory exists, so the pool never ends up with allocated slots that are
     * missing from the free list (those would be lost until destroy).
     */
    if (!free_list_reserve(pool, pool->chunk_slots)) {
        return (size_t)-1;
    }

    chunk = (dx_chunk_t *)calloc(1, sizeof(*chunk));
    if (chunk == NULL) {
        return (size_t)-1;
    }
    chunk->slots = pool->chunk_slots;
    chunk->slot_size = pool->slot_size;
    chunk->bits = (pool->chunk_slots + 7u) / 8u;

    chunk->mem = (unsigned char *)calloc(pool->chunk_slots, pool->slot_size);
    if (chunk->mem == NULL) {
        free(chunk);
        return (size_t)-1;
    }
    chunk->in_use = (unsigned char *)calloc(chunk->bits, 1u);
    if (chunk->in_use == NULL) {
        free(chunk->mem);
        free(chunk);
        return (size_t)-1;
    }

    chunk->next = NULL;
    *tail = chunk;
    pool->total_slots += pool->chunk_slots;

    /* Cannot fail: free_list_reserve guaranteed the capacity. */
    for (size_t i = 0; i < pool->chunk_slots; i++) {
        (void)free_list_push(pool, (uint32_t)index, (uint32_t)i);
    }
    return index;
}

dx_bufpool_t *dx_bufpool_create(size_t slot_size, size_t chunk_slots)
{
    dx_bufpool_t *pool;

    if (slot_size == 0 || chunk_slots == 0 || chunk_slots > 0xffffffffu) {
        return NULL;
    }

    pool = (dx_bufpool_t *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->slot_size = slot_size;
    pool->chunk_slots = chunk_slots;
    return pool;
}

void dx_bufpool_destroy(dx_bufpool_t *pool)
{
    dx_chunk_t *chunk;

    if (pool == NULL) {
        return;
    }
    chunk = pool->chunks;
    while (chunk != NULL) {
        dx_chunk_t *next = chunk->next;

        free(chunk->mem);
        free(chunk->in_use);
        free(chunk);
        chunk = next;
    }
    free(pool->free_list);
    free(pool);
}

unsigned char *dx_bufpool_acquire(dx_bufpool_t *pool, size_t min_size)
{
    uint64_t key;
    uint32_t chunk_index;
    uint32_t slot_index;
    dx_chunk_t *chunk = NULL;
    dx_chunk_t *it;
    size_t seen = 0;

    if (pool == NULL || min_size > pool->slot_size) {
        return NULL;
    }

    while (pool->free_count == 0) {
        if (bufpool_add_chunk(pool) == (size_t)-1) {
            return NULL; /* out of memory */
        }
    }

    key = pool->free_list[--pool->free_count];
    chunk_index = (uint32_t)(key >> 32);
    slot_index = (uint32_t)(key & 0xffffffffu);

    for (it = pool->chunks; it != NULL; it = it->next) {
        if (seen == (size_t)chunk_index) {
            chunk = it;
            break;
        }
        seen++;
    }
    if (chunk == NULL || (size_t)slot_index >= chunk->slots) {
        /*
         * The free list should never contain a stale entry. Rather than
         * corrupt the pool, put the entry back and report exhaustion.
         */
        if (!free_list_push(pool, chunk_index, slot_index)) {
            /* Cannot happen: the list just shrank, so a push always fits. */
        }
        return NULL;
    }
    if (bit_test(chunk->in_use, slot_index)) {
        /* Defensive: a slot marked in use must never appear on the free list. */
        (void)free_list_push(pool, chunk_index, slot_index);
        return NULL;
    }

    bit_set(chunk->in_use, slot_index);
    pool->outstanding++;
    return chunk->mem + ((size_t)slot_index * chunk->slot_size);
}

bool dx_bufpool_release(dx_bufpool_t *pool, const unsigned char *data)
{
    dx_chunk_t *chunk;
    size_t ordinal = 0;
    size_t offset;
    size_t slot_index;

    if (pool == NULL || data == NULL) {
        return false;
    }

    for (chunk = pool->chunks; chunk != NULL; chunk = chunk->next) {
        if (data >= chunk->mem && data < chunk->mem + (chunk->slots * chunk->slot_size)) {
            break;
        }
        ordinal++;
    }
    if (chunk == NULL) {
        return false; /* not ours */
    }

    offset = (size_t)(data - chunk->mem);
    if (offset % chunk->slot_size != 0) {
        return false; /* not a slot base address */
    }
    slot_index = offset / chunk->slot_size;
    if (slot_index >= chunk->slots || !bit_test(chunk->in_use, slot_index)) {
        return false; /* double release */
    }

    bit_clear(chunk->in_use, slot_index);
    if (pool->outstanding > 0) {
        pool->outstanding--;
    }
    if (!free_list_push(pool, (uint32_t)ordinal, (uint32_t)slot_index)) {
        /*
         * Out of memory for the free list. Marking the slot as owned is the
         * safe failure mode: it stays allocated but unusable, which is
         * detected by the status counters instead of corrupting memory.
         */
        bit_set(chunk->in_use, slot_index);
        if (pool->outstanding < pool->total_slots) {
            pool->outstanding++;
        }
        return false;
    }
    return true;
}

size_t dx_bufpool_outstanding(const dx_bufpool_t *pool)
{
    return (pool == NULL) ? 0u : pool->outstanding;
}

size_t dx_bufpool_total_slots(const dx_bufpool_t *pool)
{
    return (pool == NULL) ? 0u : pool->total_slots;
}
