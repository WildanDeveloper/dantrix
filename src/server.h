/*
 * server.h - Process level wiring: signal handling, listener creation and the
 * single worker lifecycle.
 */
#ifndef DX_SERVER_H
#define DX_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "file.h"
#include "http_parser.h"
#include "log.h"
#include "mime.h"
#include "worker.h"

typedef struct {
    const char *bind_addr;      /* IP literal; NULL means the built-in default */
    int port;                   /* 1..65535 */
    int backlog;                /* listen() queue; clamped to >= 1 */
    size_t max_conns;           /* hard connection cap (per worker) */
    int workers;                /* 1 = main-thread loop; >1 = worker threads */
    bool reuseport;             /* one SO_REUSEPORT listener per worker */
    bool cpu_affinity;          /* pin worker i to CPU i */
    dx_http_limits_t limits;    /* HTTP request line / header limits */
    dx_log_t *log;              /* borrowed error logger */
    dx_log_t *access_log;       /* borrowed access logger; NULL disables logging */
    dx_log_format_t log_format; /* common or combined */
    /* Static files. */
    dx_fs_t *fs;                  /* document root handle; NULL serves no files */
    dx_mime_t *mime;              /* extension mapping; NULL means octet-stream */
    dx_static_opts_t static_opts; /* sendfile, etag, cache-control, ... */
    /* Connection lifecycle. */
    dx_lifecycle_opts_t life; /* keep-alive budget and timeouts */
} dx_server_config_t;

/*
 * Run the server until SIGINT or SIGTERM arrives. With workers == 1 the main
 * thread runs the event loop directly (the v0.1 behaviour); with workers > 1
 * the main thread spawns one worker thread per configured worker and waits on
 * sigwait(), waking the workers through their eventfds on shutdown. Returns 0
 * on a graceful shutdown, -1 on a fatal error (errno set).
 */
int dx_server_run(const dx_server_config_t *cfg);

/* True once SIGINT or SIGTERM has been received. Signal safe. */
int dx_server_stopping(void);

#endif /* DX_SERVER_H */
