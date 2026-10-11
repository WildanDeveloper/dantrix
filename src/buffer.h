/*
 * buffer.h - Growable byte buffer plus the slab pool that hands out its memory.
 *
 * The buffer keeps three offsets into one allocation: data (base), cap
 * (allocated size), and the pair (off, len) describing the live window
 * [off, len). Nothing in a connection ever calls malloc on the hot path:
 * buffers are acquired from dx_bufpool_t and returned when the connection is
 * released.
 */
#ifndef DX_BUFFER_H
#define DX_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    unsigned char *data; /* owned by the pool while attached; NULL when empty */
    size_t cap;          /* allocated bytes reachable from data */
    size_t off;          /* first byte that has not been consumed yet */
    size_t len;          /* one past the last valid byte */
} dx_buffer_t;

/* Attach caller-owned storage (typically a pooled slot) to a buffer. */
void dx_buffer_init(dx_buffer_t *b, unsigned char *data, size_t cap);

/* Detach the buffer, keeping the offsets so they can be restored. */
void dx_buffer_detach(dx_buffer_t *b);

/* Number of valid bytes in [off, len). */
size_t dx_buffer_readable(const dx_buffer_t *b);

/* Number of bytes that can still be written without growing. */
size_t dx_buffer_writable(const dx_buffer_t *b);

/*
 * Mark n bytes as consumed. n must not exceed dx_buffer_readable(); the
 * function clamps instead of trusting the caller, because a wrong offset
 * would corrupt the next read.
 */
void dx_buffer_consume(dx_buffer_t *b, size_t n);

/* Move the live window to the front of the allocation. */
void dx_buffer_compact(dx_buffer_t *b);

/*
 * Append n bytes. Compacts first if needed and returns false when the buffer
 * cannot grow (it never grows by itself: the caller decides the new size).
 */
bool dx_buffer_append(dx_buffer_t *b, const void *src, size_t n);

/*
 * Fixed size buffer pool.
 *
 * Slots are carved out of chunks so that the steady state performs no
 * allocation at all. Ownership is tracked with a per-chunk bitmap, which lets
 * dx_bufpool_release reject a double release instead of handing the same
 * memory to two owners.
 */
typedef struct dx_bufpool dx_bufpool_t;

/*
 * Create a pool of slots of slot_size bytes, allocating chunk_slots slots at a
 * time. Returns NULL on invalid arguments or allocation failure.
 */
dx_bufpool_t *dx_bufpool_create(size_t slot_size, size_t chunk_slots);

/* Release the pool and all of its chunks. Outstanding buffers are freed. */
void dx_bufpool_destroy(dx_bufpool_t *pool);

/*
 * Acquire a slot of at least min_size bytes, or NULL when min_size exceeds the
 * pool slot size or allocation fails.
 */
unsigned char *dx_bufpool_acquire(dx_bufpool_t *pool, size_t min_size);

/*
 * Return a slot previously obtained from this pool. Returns false when the
 * pointer does not belong to the pool or is not currently owned (double
 * release); the pool is left untouched in that case.
 */
bool dx_bufpool_release(dx_bufpool_t *pool, const unsigned char *data);

/* Diagnostics used by tests and the status endpoint. */
size_t dx_bufpool_outstanding(const dx_bufpool_t *pool);
size_t dx_bufpool_total_slots(const dx_bufpool_t *pool);

#endif /* DX_BUFFER_H */
