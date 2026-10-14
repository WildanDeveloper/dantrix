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
#include "config.h"
#include "connlimit.h"
#include "conn.h"
#include "dantrix.h"
#include "file.h"
#include "http_parser.h"
#include "log.h"
#include "mime.h"
#include "status.h"

/* Static file serving options (v0.3), owned by the caller. */
typedef struct {
    bool sendfile;             /* sendfile() when true, pread()+send() otherwise */
    size_t sendfile_chunk;     /* max bytes per sendfile call (fairness) */
    bool etag;                 /* send and evaluate ETag */
    bool server_tokens;        /* "Server: Dantrix/x.y.z" when true */
    bool hidden_files;         /* serve dot segments when true */
    const char *cache_control; /* Cache-Control value, "" to omit */
    /*
     * Extra response headers to add to every response, as "Name: value" strings
     * without the CRLF. Borrowed from the configuration, which owns the storage;
     * add_header_count == 0 disables the feature.
     */
    char (*add_header)[DX_CFG_STR_MAX];
    size_t add_header_count;
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

/* Forward declaration: the reload snapshot is defined after dx_worker_t. */
typedef struct dx_worker_reload dx_worker_reload_t;

/*
 * Called by the worker when it is asked to reopen the log files. Only the
 * single-worker path sets it: there the loop runs on the main thread, so the
 * callback owns the FILE streams. In multi-worker mode the main thread reads
 * the signal itself and never forwards REOPEN.
 */
typedef void (*dx_reopen_fn)(void *ctx);

typedef struct dx_worker {
    int epfd;                               /* epoll instance, -1 when not running */
    int listen_fd;                          /* listening socket, -1 when not running */
    int ctl_fd;                             /* borrowed control fd, -1 when not running */
    bool ctl_is_signal;                     /* ctl_fd is a signalfd, not an eventfd */
    dx_pool_t *pool;                        /* connection slots */
    dx_bufpool_t *bufs;                     /* per-connection read buffers */
    dx_bufpool_t *hdrs;                     /* per-connection HTTP header tables */
    dx_fcache_t *fcache;                    /* open-file cache; NULL = disabled */
    dx_connlimit_t *climit;                 /* shared per-IP limit; NULL = off */
    unsigned max_conns_per_ip;              /* 0 = unlimited */
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
    bool force_stop;                        /* SIGQUIT: close now, do not drain */
    bool owns_listen_fd;                    /* close the listener on destroy */
    bool exclusive_listen;                  /* EPOLLEXCLUSIVE on a shared listener */
    unsigned worker_id;                     /* index within the worker group */
    dx_worker_reload_t *reload;             /* borrowed; NULL = no hot reload */
    dx_reopen_fn reopen_cb;                 /* single-worker reopen hook */
    void *reopen_ctx;                       /* passed to reopen_cb */
    int (*reload_cb)(const void *ctx, dx_worker_reload_t *reload); /* refresh snapshot */
    void *reload_ctx;                                              /* passed to reload_cb */
    dx_status_t *status;     /* borrowed shared counters; NULL = off */
    const char *status_path; /* JSON endpoint path; NULL = off */
    uint64_t start_ms;       /* for uptime in the status endpoint */
    unsigned total_workers;  /* for the status endpoint */
    uint64_t accepted_total;
    uint64_t rejected_total;
    uint64_t closed_total;
} dx_worker_t;

/*
 * Signals and shutdown commands share one channel per worker: the eventfd
 * payload is a bitmask, so the main thread can ask for several things at once
 * and a signalfd-driven worker forwards the same values.
 */
enum {
    DX_CMD_SHUTDOWN = 1u, /* graceful drain, then exit */
    DX_CMD_RELOAD = 2u,   /* copy the reloadable settings from the snapshot */
    DX_CMD_STATS = 4u,    /* write a statistics line to the error log */
    DX_CMD_REOPEN = 8u,   /* ask the orchestrator to reopen its log files */
    DX_CMD_FORCE = 16u    /* close everything now, no graceful drain */
};

/*
 * The reloadable part of the configuration. The main thread writes it under
 * the mutex when the operator reloads; a worker copies it into its own fields
 * under the same mutex and never touches the shared struct afterwards, so the
 * hot path stays lock-free.
 */
typedef struct dx_worker_reload {
    pthread_mutex_t mu;
    dx_static_opts_t static_opts;
    dx_lifecycle_opts_t life;
    dx_http_limits_t limits;
} dx_worker_reload_t;

/*
 * Options passed to dx_worker_init().
 */
typedef struct {
    int listen_fd;
    int ctl_fd; /* eventfd (multi-worker) or signalfd (single) */
    bool ctl_is_signal;
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
    /*
     * Open-file cache sizing. slots == 0 (or valid_ms == 0) disables the cache
     * and keeps the open+fstat-per-request path.
     */
    size_t fcache_slots;
    uint64_t fcache_valid_ms;
    dx_connlimit_t *climit;    /* borrowed shared table; NULL = off */
    unsigned max_conns_per_ip; /* 0 = unlimited */
    const dx_static_opts_t *static_opts;
    const dx_lifecycle_opts_t *life;
    dx_worker_reload_t *reload;
    dx_reopen_fn reopen_cb;
    void *reopen_ctx;
    int (*reload_cb)(const void *ctx, dx_worker_reload_t *reload);
    void *reload_ctx;
    dx_status_t *status;     /* shared counters; NULL = endpoint disabled */
    const char *status_path; /* JSON endpoint path; NULL = disabled */
    uint64_t start_ms;       /* status endpoint uptime base */
    unsigned total_workers;  /* status endpoint worker count */
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
 * 404). ctl_fd, when not negative, is registered in the epoll set: an eventfd
 * carries a DX_CMD_* bitmask, a signalfd is decoded into the same commands.
 * Returns 0 or -1.
 */
int dx_worker_init(dx_worker_t *w, const dx_worker_opts_t *opts);

/* Release every resource the worker owns. Safe to call after a failed init. */
void dx_worker_destroy(dx_worker_t *w);

/*
 * Run the event loop until a stop is requested (the signal flag, a wake_fd
 * wakeup or the signalfd). Connections still open at that point are closed.
 * Returns 0 on a clean stop, -1 on a fatal epoll error.
 */
int dx_worker_run(dx_worker_t *w);

/* Snapshot the per-worker counters. */
void dx_worker_stats(const dx_worker_t *w, dx_worker_stats_t *out);

/* Copy the current reload snapshot into this worker. */
void dx_worker_apply_reload(dx_worker_t *w);

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

/*
 * The control channel (an eventfd in multi-worker mode, a signalfd in
 * single-worker mode) uses the WAKE tag kind. The two are never registered at
 * the same time: single-worker has no wake_fd and multi-worker has no
 * signal_fd, so one tag kind is enough and the 2-bit kind field stays as is.
 */
static inline uint64_t dx_tag_wake(void)
{
    return (uint64_t)DX_TAG_KIND_WAKE << DX_TAG_KIND_SHIFT;
}

static inline uint64_t dx_tag_listener(void)
{
    return (uint64_t)DX_TAG_KIND_LISTENER << DX_TAG_KIND_SHIFT;
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
