/*
 * worker.h - The event loop. One worker owns one epoll instance, one listener,
 * one connection pool and one buffer pool. Nothing in a worker is shared with
 * another worker, so there are no locks anywhere on this path.
 */
#ifndef DX_WORKER_H
#define DX_WORKER_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "conn.h"
#include "dantrix.h"
#include "file.h"
#include "http_parser.h"
#include "log.h"
#include "mime.h"

/* Static file serving options (v0.3), owned by the caller. */
typedef struct {
    bool sendfile;             /* sendfile() when true, pread()+send() otherwise */
    size_t sendfile_chunk;     /* max bytes per sendfile call (fairness) */
    bool etag;                 /* send and evaluate ETag */
    bool server_tokens;        /* "Server: Dantrix/x.y.z" when true */
    bool hidden_files;         /* serve dot segments when true */
    const char *cache_control; /* Cache-Control value, "" to omit */
} dx_static_opts_t;

/* Connection lifecycle options (v0.4), durations in milliseconds. */
typedef struct {
    size_t keepalive_requests;     /* requests before the connection closes */
    uint64_t header_timeout_ms;    /* limit for a complete request header block */
    uint64_t body_timeout_ms;      /* limit between body chunks */
    uint64_t send_timeout_ms;      /* limit between write progress steps */
    uint64_t keepalive_timeout_ms; /* idle time before the socket is closed */
    uint64_t shutdown_timeout_ms;  /* graceful shutdown budget */
    uint64_t max_body_size;        /* request body bytes read and discarded */
} dx_lifecycle_opts_t;

typedef struct dx_worker {
    int epfd;                               /* epoll instance, -1 when not running */
    int listen_fd;                          /* listening socket, -1 when not running */
    int wake_fd;                            /* borrowed eventfd, -1 when not running */
    dx_pool_t *pool;                        /* connection slots */
    dx_bufpool_t *bufs;                     /* per-connection read buffers */
    dx_log_t *log;                          /* borrowed error logger */
    dx_log_t *access_log;                   /* borrowed access logger; NULL = off */
    dx_log_format_t log_format;             /* common or combined */
    dx_http_limits_t limits;                /* HTTP header limits */
    dx_fs_t *fs;                            /* document root, borrowed */
    dx_mime_t *mime;                        /* MIME mapping, borrowed */
    dx_static_opts_t static_opts;           /* static serving options */
    dx_lifecycle_opts_t life;               /* keep-alive and timeout settings */
    dx_timer_wheel_t *timers;               /* one timer node per connection */
    dx_timer_node_t accept_timer;           /* deferred accept retry after EMFILE */
    bool draining;                          /* shutdown: listener is detached */
    uint64_t shutdown_deadline_ms;          /* when draining started */
    size_t max_conns;                       /* mirrors the pool cap for quick checks */
    const volatile sig_atomic_t *stop_flag; /* borrowed; NULL means never stop */
    bool stop_requested;                    /* set by the wake_fd handler */
    bool owns_listen_fd;                    /* close the listener on destroy */
    bool exclusive_listen;                  /* EPOLLEXCLUSIVE on a shared listener */
    unsigned worker_id;                     /* index within the worker group */
    uint64_t accepted_total;
    uint64_t rejected_total;
    uint64_t closed_total;
} dx_worker_t;

/*
 * Everything dx_worker_init() needs. Every pointer is borrowed: the worker
 * takes ownership only of the epoll, the pools and (when owns_listen_fd is
 * true) the listener descriptor. wake_fd is always borrowed: the caller keeps
 * it so that it can keep signalling a worker that already stopped.
 */
typedef struct {
    int listen_fd;
    int wake_fd;
    unsigned worker_id;
    size_t max_conns;
    bool owns_listen_fd;
    bool exclusive_listen;
    dx_log_t *log;
    dx_log_t *access_log;
    dx_log_format_t log_format;
    const volatile sig_atomic_t *stop_flag;
    const dx_http_limits_t *limits;
    dx_fs_t *fs;
    dx_mime_t *mime;
    const dx_static_opts_t *static_opts;
    const dx_lifecycle_opts_t *life;
} dx_worker_opts_t;

/* Per-worker counters snapshot, taken after the worker thread has joined. */
typedef struct {
    uint64_t accepted;
    uint64_t rejected;
    uint64_t closed;
} dx_worker_stats_t;

/*
 * Initialize a worker. fs, mime and the option structs are borrowed for the
 * lifetime of the worker; a NULL fs disables file serving (every path answers
 * 404). wake_fd, when not negative, is registered in the epoll set and any
 * wakeup on it is treated as a stop request. Returns 0 or -1.
 */
int dx_worker_init(dx_worker_t *w, const dx_worker_opts_t *opts);

/* Release every resource the worker owns. Safe to call after a failed init. */
void dx_worker_destroy(dx_worker_t *w);

/*
 * Run the event loop until the stop flag becomes non-zero (single-worker mode)
 * or a wake_fd wakeup arrives (worker-thread mode). Connections still open at
 * that point are closed. Returns 0 on a clean stop, -1 on a fatal epoll error.
 */
int dx_worker_run(dx_worker_t *w);

/* Snapshot the per-worker counters. */
void dx_worker_stats(const dx_worker_t *w, dx_worker_stats_t *out);

/* Logger accessor used by conn.c (the logger is borrowed, never owned). */
dx_log_t *dx_worker_log(dx_worker_t *w);

/*
 * Write the access log line for the request recorded on conn, if any, and
 * clear the record so the request is never logged twice. Called by the
 * connection flow when a response completes.
 */
void dx_worker_emit_access(dx_worker_t *w, dx_conn_t *conn);

/*
 * Event tag helpers. A tag is (kind, generation, index) packed into the epoll
 * user data word; see dantrix.h for the bit layout.
 */
enum {
    DX_TAG_KIND_INVALID = 0u,
    DX_TAG_KIND_LISTENER = 1u,
    DX_TAG_KIND_CONN = 2u,
    DX_TAG_KIND_WAKE = 3u
};

static inline uint64_t dx_tag_listener(void)
{
    return (uint64_t)DX_TAG_KIND_LISTENER << DX_TAG_KIND_SHIFT;
}

static inline uint64_t dx_tag_wake(void)
{
    return (uint64_t)DX_TAG_KIND_WAKE << DX_TAG_KIND_SHIFT;
}

static inline uint64_t dx_tag_conn(uint32_t index, uint32_t generation)
{
    return ((uint64_t)DX_TAG_KIND_CONN << DX_TAG_KIND_SHIFT) |
           (((uint64_t)generation & DX_TAG_GEN_MASK) << DX_TAG_GEN_SHIFT) |
           ((uint64_t)index & DX_TAG_INDEX_MASK);
}

static inline unsigned dx_tag_kind(uint64_t tag)
{
    return (unsigned)((tag >> DX_TAG_KIND_SHIFT) & 3u);
}

static inline uint32_t dx_tag_index(uint64_t tag)
{
    return (uint32_t)(tag & DX_TAG_INDEX_MASK);
}

static inline uint32_t dx_tag_generation(uint64_t tag)
{
    return (uint32_t)((tag >> DX_TAG_GEN_SHIFT) & DX_TAG_GEN_MASK);
}

#endif /* DX_WORKER_H */
