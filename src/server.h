/*
 * server.h - Process level wiring: signal handling, listener creation and the
 * single worker lifecycle for v0.1.
 */
#ifndef DX_SERVER_H
#define DX_SERVER_H

#include <stddef.h>

#include "log.h"

typedef struct {
    const char *bind_addr; /* IP literal; NULL means the built-in default */
    int port;              /* 1..65535 */
    int backlog;           /* listen() queue; clamped to >= 1 */
    size_t max_conns;      /* hard connection cap */
    dx_log_t *log;         /* borrowed */
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
