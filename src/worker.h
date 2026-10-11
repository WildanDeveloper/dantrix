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

typedef struct dx_worker {
    int epfd;                               /* epoll instance, -1 when not running */
    int listen_fd;                          /* listening socket, -1 when not running */
    dx_pool_t *pool;                        /* connection slots */
    dx_bufpool_t *bufs;                     /* per-connection read buffers */
    dx_log_t *log;                          /* borrowed error logger */
    dx_log_t *access_log;                   /* borrowed access logger; NULL = off */
    dx_log_format_t log_format;             /* common or combined */
    dx_http_limits_t limits;                /* HTTP header limits */
    dx_fs_t *fs;                            /* document root, borrowed */
    dx_mime_t *mime;                        /* MIME mapping, borrowed */
    dx_static_opts_t static_opts;           /* static serving options */
    size_t max_conns;                       /* mirrors the pool cap for quick checks */
    const volatile sig_atomic_t *stop_flag; /* borrowed; NULL means never stop */
    uint64_t accepted_total;
    uint64_t rejected_total;
    uint64_t closed_total;
} dx_worker_t;

/*
 * Initialize a worker around an already listening descriptor. fs, mime and
 * the static options are borrowed for the lifetime of the worker; a NULL fs
 * disables file serving (every path answers 404). Returns 0 or -1.
 */
int dx_worker_init(dx_worker_t *w, int listen_fd, size_t max_conns, dx_log_t *log,
                   const volatile sig_atomic_t *stop_flag, const dx_http_limits_t *limits,
                   dx_log_t *access_log, dx_log_format_t log_format, dx_fs_t *fs, dx_mime_t *mime,
                   const dx_static_opts_t *static_opts);

/* Release every resource the worker owns. Safe to call after a failed init. */
void dx_worker_destroy(dx_worker_t *w);

/*
 * Run the event loop until the stop flag becomes non-zero. Connections still
 * open at that point are closed. Returns 0 on a clean stop, -1 on a fatal
 * epoll error.
 */
int dx_worker_run(dx_worker_t *w);

/* Logger accessor used by conn.c (the logger is borrowed, never owned). */
dx_log_t *dx_worker_log(dx_worker_t *w);

/*
 * Event tag helpers. A tag is (kind, generation, index) packed into the epoll
 * user data word; see dantrix.h for the bit layout.
 */
enum { DX_TAG_KIND_INVALID = 0u, DX_TAG_KIND_LISTENER = 1u, DX_TAG_KIND_CONN = 2u };

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
