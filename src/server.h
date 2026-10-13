/*
 * server.h - Process level wiring: log ownership, signal handling, listener
 * creation and the worker lifecycle.
 */
#ifndef DX_SERVER_H
#define DX_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "file.h"
#include "http_parser.h"
#include "mime.h"
#include "net.h"
#include "worker.h"

typedef struct {
    const char *bind_addr;      /* IP literal; NULL means the built-in default */
    int port;                   /* 1..65535 */
    int backlog;                /* listen() queue; clamped to >= 1 */
    size_t max_conns;           /* hard connection cap (per worker) */
    int workers;                /* 1 = main-thread loop; >1 = worker threads */
    bool reuseport;             /* one SO_REUSEPORT listener per worker */
    bool cpu_affinity;          /* pin worker i to CPU i */
    dx_sockopts_t sockopts;     /* listener tuning; zeroed = kernel defaults */
    dx_http_limits_t limits;    /* HTTP request line / header limits */
    dx_log_format_t log_format; /* common or combined */
    dx_log_level_t log_level;   /* error, warn, info, debug */
    /*
     * Log destinations: "stderr", "stdout", "off" or a file path. The server
     * owns the streams and the loggers, so SIGUSR1 can reopen them in place.
     */
    const char *error_log_path;
    const char *access_log_path;
    /* Static files. */
    dx_fs_t *fs;                  /* document root handle; NULL serves no files */
    dx_mime_t *mime;              /* extension mapping; NULL means octet-stream */
    dx_static_opts_t static_opts; /* sendfile, etag, cache-control, ... */
    /* Open-file cache sizing (0 = disabled, the pre-v0.7.3 behaviour). */
    size_t fcache_slots;
    uint64_t fcache_valid_ms;
    /* Connection lifecycle. */
    dx_lifecycle_opts_t life; /* keep-alive budget and timeouts */
    /* Process and privileges (all optional; empty/NULL = off). */
    bool daemonize;          /* fork into the background */
    const char *pid_file;    /* write the PID here */
    const char *user;        /* drop to this user after bind */
    const char *group;       /* drop to this group after bind */
    const char *status_path; /* JSON status endpoint path; empty = disabled */

    /*
     * Optional SIGHUP hook. The callback re-reads the configuration sources
     * (file, environment, CLI overrides) and returns 0 after updating *reload
     * with the freshly reloadable settings. It runs on the thread that owns
     * the control channel, so it may allocate but must not block for long.
     */
    int (*reload_cb)(const void *ctx, dx_worker_reload_t *reload);
    void *reload_ctx;
} dx_server_config_t;

/*
 * Run the server until a stop signal arrives. With workers == 1 the main
 * thread runs the event loop and reads a signalfd; with workers > 1 the main
 * thread owns the signalfd and forwards commands to worker threads through
 * their eventfds.
 *
 * Handled signals: SIGTERM/SIGINT graceful shutdown, SIGQUIT immediate close,
 * SIGHUP reload of the reloadable settings, SIGUSR1 reopen the log files,
 * SIGUSR2 statistics snapshot.
 *
 * Returns 0 on a graceful shutdown, -1 on a fatal error (errno set).
 */
int dx_server_run(const dx_server_config_t *cfg);

#endif /* DX_SERVER_H */
