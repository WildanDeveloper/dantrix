/*
 * conn.h - Connection slots, the slot pool and per-connection HTTP flow.
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
#include "http_parser.h"
#include "timer.h"

typedef enum {
    DX_CONN_FREE = 0, /* slot is in the free list, owns nothing */
    DX_CONN_READING,  /* reading request headers (or discarding a body) */
    DX_CONN_WRITING,  /* response pending, EPOLLOUT registered */
    DX_CONN_IDLE,     /* keep-alive: waiting for the next request, buffer free */
    DX_CONN_CLOSING   /* being torn down */
} dx_conn_state_t;

struct dx_worker; /* defined in worker.h */

/*
 * What the access logger needs about the request that was served. Strings are
 * bounded copies of client input; escaping happens at log write time.
 */
typedef struct {
    int status; /* 0 until a request reaches a response */
    char method[16];
    char target[256];
    char referer[128];
    char user_agent[128];
    size_t body_bytes;
    bool http10; /* HTTP/1.0 when true, HTTP/1.1 otherwise */
} dx_access_t;

typedef struct {
    int fd;
    uint32_t index;      /* slot index inside the pool */
    uint32_t generation; /* bumped on every acquire */
    uint64_t id;         /* stable ordinal for logging, never reused */
    dx_conn_state_t state;
    bool eof;               /* peer sent FIN: finish the reply, then close */
    bool close_conn;        /* close once the current response has been fully sent */
    bool response_pending;  /* response bytes sit in buf and are not all sent */
    dx_buffer_t buf;        /* request bytes, then the response headers */
    unsigned char *buf_mem; /* pool slot to hand back, NULL when malloc'd */
    unsigned char *hold;    /* pipelined bytes parked while a response is staged */
    size_t hold_len;
    int file_fd;           /* file being streamed, -1 when none */
    uint64_t file_offset;  /* next byte to read from file_fd */
    uint64_t file_left;    /* response body bytes still to send */
    uint64_t body_left;    /* request body bytes still to be discarded */
    size_t body_offset;    /* where the body starts inside the read buffer */
    uint64_t requests;     /* requests already answered on this connection */
    dx_timer_node_t timer; /* the single timer this connection owns */
    dx_http_parser_t parser;
    dx_access_t acc;
    char peer[64]; /* formatted client address, "-" when unknown */
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
/* Per-connection HTTP flow                                               */
/* --------------------------------------------------------------------- */

/* Prepare a freshly acquired slot for a connected descriptor. */
void dx_conn_init(dx_conn_t *conn, int fd, const dx_http_limits_t *limits);

/* Return the read buffer (pool slot or heap) and reset counters. */
void dx_conn_release_buffer(dx_conn_t *conn, dx_bufpool_t *pool);

/* Close a streamed file, if any. Safe to call at any time. */
void dx_conn_release_file(dx_conn_t *conn);

/* Release pipelined bytes parked while a response was staged. */
void dx_conn_release_hold(dx_conn_t *conn);

/*
 * Record a status code for the access log without going through a normal
 * response (used by the 408 written on a header timeout).
 */
void dx_conn_record_status(dx_conn_t *conn, int status);

/* Reset pointers and state without changing the generation. */
void dx_conn_reset(dx_conn_t *conn);

/* True while response bytes are waiting to be written out. */
bool dx_conn_has_pending(const dx_conn_t *conn);

/* True while header bytes or file bytes are still to be sent. */
bool dx_conn_has_work(const dx_conn_t *conn);

/*
 * Read whatever is available and run the HTTP flow (parse, dispatch, respond).
 * Returns 0 while the connection stays open, -1 when the caller must close it.
 */
int dx_conn_on_readable(struct dx_worker *w, dx_conn_t *conn);

/* Drain the pending response. Returns 0 or -1 (close). */
int dx_conn_on_writable(struct dx_worker *w, dx_conn_t *conn);

/*
 * Arm a read buffer when the connection is idle and the peer started sending.
 * The idle state holds no buffer, so the slot is taken back here.
 * Returns 0 on success, -1 when no buffer is available.
 */
int dx_conn_ensure_buffer(struct dx_worker *w, dx_conn_t *conn);

/*
 * Refresh the connection timer for its current state: header timeout while
 * reading, body timeout while discarding, send timeout while writing, and the
 * keep-alive timeout while idle. Never fails.
 */
void dx_conn_arm_timer(struct dx_worker *w, dx_conn_t *conn);

/* Milliseconds left on the connection timer, or -1 when it is not armed. */
int64_t dx_conn_timer_left_ms(const struct dx_worker *w, const dx_conn_t *conn);

#endif /* DX_CONN_H */
