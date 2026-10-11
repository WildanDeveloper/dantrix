/*
 * conn.h - Connection slots, the slot pool and per-connection I/O.
 *
 * A connection object is a slot in a pool. Slots are recycled, never freed, and
 * every reuse bumps a generation counter that the event loop stamps into the
 * epoll tag so that an event for a slot that has since been recycled is
 * recognised and dropped instead of touching the new owner's state.
 */
#ifndef DX_CONN_H
#define DX_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"

typedef enum {
    DX_CONN_FREE = 0, /* slot is in the free list, owns nothing */
    DX_CONN_READING,  /* waiting for client bytes */
    DX_CONN_WRITING,  /* output pending, EPOLLOUT registered */
    DX_CONN_CLOSING   /* being torn down */
} dx_conn_state_t;

struct dx_worker; /* defined in worker.h */

typedef struct {
    int fd;
    uint32_t index;      /* slot index inside the pool */
    uint32_t generation; /* bumped on every acquire */
    uint64_t id;         /* stable ordinal for logging, never reused */
    dx_conn_state_t state;
    bool eof;               /* peer sent FIN; echo remains, then close */
    dx_buffer_t buf;        /* read buffer plus the pending echo window */
    unsigned char *buf_mem; /* pool slot to hand back, NULL when detached */
    uint64_t bytes_in;
    uint64_t bytes_out;
} dx_conn_t;

/* --------------------------------------------------------------------- */
/* Slot pool                                                              */
/* --------------------------------------------------------------------- */

typedef struct dx_pool dx_pool_t;

/*
 * Create a pool that allocates conn_slots objects per chunk and refuses to
 * hand out more than max_conns at a time. Returns NULL on invalid arguments or
 * allocation failure.
 */
dx_pool_t *dx_pool_create(size_t conn_slots, size_t max_conns);

/* Destroy the pool. Outstanding connections are freed with it. */
void dx_pool_destroy(dx_pool_t *pool);

/*
 * Take a free slot. The slot is marked DX_CONN_READING, its generation is
 * bumped and all byte counters are reset. Returns NULL when max_conns is
 * reached or when memory is exhausted.
 */
dx_conn_t *dx_pool_acquire(dx_pool_t *pool);

/*
 * Give a slot back. The caller must already have closed the descriptor and
 * returned the read buffer. Returns false when the slot was not owned
 * (double release), leaving the pool untouched.
 */
bool dx_pool_release(dx_pool_t *pool, dx_conn_t *conn);

/*
 * Resolve an event tag. Returns NULL when the slot is free or when the
 * generation does not match, which is how stale events are discarded.
 */
dx_conn_t *dx_pool_lookup(dx_pool_t *pool, uint32_t index, uint32_t generation);

/* Address of a slot by index regardless of state; NULL when out of range. */
dx_conn_t *dx_pool_slot(dx_pool_t *pool, size_t index);

/* Number of connections currently handed out. */
size_t dx_pool_active(const dx_pool_t *pool);

/* --------------------------------------------------------------------- */
/* Per-connection I/O                                                     */
/* --------------------------------------------------------------------- */

/* Prepare a freshly acquired slot for a connected descriptor. */
void dx_conn_init(dx_conn_t *conn, int fd);

/* Reset counters and state without changing the generation. */
void dx_conn_reset(dx_conn_t *conn);

/*
 * Read whatever is available and echo it back. Returns 0 when the connection
 * is still healthy, -1 when the caller must close it.
 */
int dx_conn_on_readable(struct dx_worker *w, dx_conn_t *conn);

/* Drain the pending output window. Returns 0 or -1. */
int dx_conn_on_writable(struct dx_worker *w, dx_conn_t *conn);

/* True while bytes are waiting to be written out. */
bool dx_conn_has_pending(const dx_conn_t *conn);

#endif /* DX_CONN_H */
