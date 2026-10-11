/*
 * server.h - Process level wiring: signal handling, listener creation and the
 * single worker lifecycle.
 */
#ifndef DX_SERVER_H
#define DX_SERVER_H

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
    size_t max_conns;           /* hard connection cap */
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
 * Install signal handlers, create the listener and run the worker until
 * SIGINT or SIGTERM arrives. Returns 0 on graceful shutdown, -1 on a fatal
 * error (errno set).
 */
int dx_server_run(const dx_server_config_t *cfg);

/* True once SIGINT or SIGTERM has been received. Signal safe. */
int dx_server_stopping(void);

#endif /* DX_SERVER_H */
