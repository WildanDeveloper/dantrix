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
#include "connlimit.h"
#include "fcache.h"
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
 * Size of the stack buffer a response is built in before it is staged in the
 * connection read buffer. Shared so every response path (including the one in
 * worker.c that writes a 408) sizes its staging buffer the same way.
 */
#define DX_RESPONSE_TMP 2048u

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
    int file_fd;          /* file being streamed, -1 when none */
    uint64_t file_offset; /* next byte to read from file_fd */
    uint64_t file_left;   /* response body bytes still to send */
    /*
     * When the streamed descriptor is borrowed from the worker's open-file
     * cache, this points at the pinned entry and the connection must NOT close
     * the descriptor: it unpins it instead. NULL means the connection owns
     * file_fd and closes it.
     */
    dx_fcache_entry_t *file_cache_entry;
    /*
     * In-memory response body (the directory listing). Owned by the
     * connection because it outlives the stack frame that built it,
     * and streamed through the same buffer as the headers.
     */
    unsigned char *mem_body;
    size_t mem_body_len;
    size_t mem_body_off;
    uint64_t body_left; /* request body bytes still to be discarded */
    size_t body_offset; /* where the body starts inside the read buffer */
    /*
     * Header block moved out of the read buffer while a request body is
     * drained. The parser addresses a request by offsets into that
     * block, so the copy keeps every offset valid while the read buffer
     * is reused for body bytes.
     */
    unsigned char *hdr;
    size_t hdr_len;
    uint64_t requests;     /* requests already answered on this connection */
    dx_timer_node_t timer; /* the single timer this connection owns */
    dx_http_parser_t parser;
    /*
     * Pooled header table. Held only while a request is being parsed; handed
     * back the moment the connection goes idle, so the per-connection cost is
     * the slot (a few hundred bytes) rather than the 8 KiB table it used to
     * embed. hdr_mem is the pool slot to return (NULL = none held).
     */
    dx_http_header_t *hdr_table;
    unsigned char *hdr_mem;
    /*
     * The epoll interest mask currently registered for this fd (0 = not
     * registered / unknown). epoll_ctl(MOD) is skipped when the desired mask
     * already matches, which removes one syscall from every request that is
     * read, answered and sent without the socket ever blocking.
     */
    uint32_t interest;
    dx_access_t acc;
    char peer[64]; /* formatted client address, "-" when unknown */
    /*
     * Per-IP accounting. The raw key is stored (not the formatted string) so
     * the release hashes exactly what the acquire hashed; ip_counted is false
     * when the feature is off, in which case nothing is released.
     */
    /*
     * Wall clock of the first byte of the current request, or 0 when no byte
     * has arrived yet. The header timeout is a deadline from this instant, not
     * a gap between bytes: refreshing it on every read let a client that drips
     * one byte at a time hold a connection open forever (the slowloris case),
     * which contradicted the documented behaviour.
     */
    uint64_t header_deadline_ms;
    unsigned char ip_key[16];
    uint8_t ip_key_len;
    bool ip_counted;
    /*
     * Borrowed per-IP table. Held here so dx_conn_reset() can return the
     * reservation on ANY path that recycles the slot, including the ones that
     * never reach conn_close(). Leaving the release to individual callers is
     * how a leaked reservation would turn into a permanent ban for that
     * address.
     */
    dx_connlimit_t *climit;
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

/*
 * Number of slots the pool has ever allocated, which is not the same as
 * max_conns: slots are handed out from the top of the free list, so the first
 * connections live at the highest indices of the first chunk. Anything that
 * walks every live connection (shutdown drain, statistics) must use this, not
 * the connection cap.
 */
size_t dx_pool_total_slots(const dx_pool_t *pool);

/* Number of connections currently handed out. */
size_t dx_pool_active(const dx_pool_t *pool);

/* --------------------------------------------------------------------- */
/* Per-connection HTTP flow                                               */
/* --------------------------------------------------------------------- */

/* Prepare a freshly acquired slot for a connected descriptor. */
void dx_conn_init(dx_conn_t *conn, int fd, const dx_http_limits_t *limits);

/* Return the read buffer (pool slot or heap) and reset counters. */
void dx_conn_release_buffer(dx_conn_t *conn, dx_bufpool_t *pool);

/*
 * Return the pooled HTTP header table and leave the parser without one. Must be
 * called before the slot is released (dx_conn_release_headers is idempotent).
 */
void dx_conn_release_headers(dx_conn_t *conn, dx_bufpool_t *pool);

/* Close a streamed file, if any. Safe to call at any time. */
void dx_conn_release_file(dx_conn_t *conn);

/*
 * Release the in-memory response body and the saved header block. Safe to call
 * at any time and from any state; both are plain owned allocations.
 */
void dx_conn_release_scratch(dx_conn_t *conn);

/* Release pipelined bytes parked while a response was staged. */
void dx_conn_release_hold(dx_conn_t *conn);

/*
 * Record a status code for the access log without going through a normal
 * response (used by the 408 written on a header timeout).
 */
void dx_conn_record_status(dx_conn_t *conn, int status);

/*
 * Reset pointers and state without changing the generation. Every allocation
 * the slot owns is freed or returned here, so a recycled slot never carries
 * state (or memory) from its previous owner.
 */
void dx_conn_reset(dx_conn_t *conn);

/* True while response bytes are waiting to be written out. */
bool dx_conn_has_pending(const dx_conn_t *conn);

/* True while header bytes, file bytes or a memory body are still to be sent. */
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

#endif /* DX_CONN_H */
