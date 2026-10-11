#include "server.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "dantrix.h"
#include "net.h"
#include "worker.h"

/*
 * The flag is written from a signal handler and read from the event loop. Only
 * sig_atomic_t writes of 0/1 are used, which is the exact contract the C
 * standard guarantees for this pattern.
 */
static volatile sig_atomic_t g_stop = 0;

static void on_stop_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

int dx_server_stopping(void)
{
    return g_stop != 0;
}

/* Install the v0.1 signal set. Returns 0 or -1. */
static int install_signals(void)
{
    struct sigaction sa;
    struct sigaction ignore;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    /*
     * No SA_RESTART: a signal must interrupt epoll_wait so that the loop can
     * observe the stop flag promptly instead of waiting for the next event.
     */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }

    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    ignore.sa_flags = 0;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) != 0) {
        return -1;
    }
    return 0;
}

int dx_server_run(const dx_server_config_t *cfg)
{
    dx_worker_t worker;
    int listen_fd;
    int rc = 0;

    if (cfg == NULL || cfg->log == NULL || cfg->port < 1 || cfg->port > 65535) {
        errno = EINVAL;
        return -1;
    }

    if (install_signals() != 0) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "sigaction: %s", dx_strerror(errno));
        return -1;
    }

    listen_fd = dx_listen_tcp(cfg->bind_addr, cfg->port, cfg->backlog);
    if (listen_fd < 0) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "cannot listen on %s:%d: %s", cfg->bind_addr,
                     cfg->port, dx_strerror(errno));
        return -1;
    }

    /*
     * The worker takes ownership of listen_fd: dx_worker_destroy closes it, so
     * every failure path from here on must go through destroy and never close
     * the descriptor directly (double close could hand the fd to a new
     * connection).
     */
    if (dx_worker_init(&worker, listen_fd, cfg->max_conns, cfg->log, &g_stop, &cfg->limits,
                       cfg->access_log, cfg->log_format, cfg->fs, cfg->mime, &cfg->static_opts,
                       &cfg->life) != 0) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "worker init failed: %s", dx_strerror(errno));
        dx_worker_destroy(&worker);
        return -1;
    }

    dx_log_write(cfg->log, DX_LOG_INFO, "%s %s listening on %s:%d (max %zu connections)",
                 DX_PROGRAM_NAME, DX_VERSION_STRING, cfg->bind_addr, cfg->port, cfg->max_conns);

    rc = dx_worker_run(&worker);

    if (rc == 0) {
        dx_log_write(
            cfg->log, DX_LOG_INFO, "shutdown complete (accepted %llu, rejected %llu, closed %llu)",
            (unsigned long long)worker.accepted_total, (unsigned long long)worker.rejected_total,
            (unsigned long long)worker.closed_total);
    } else {
        dx_log_write(cfg->log, DX_LOG_ERROR, "worker stopped with an error");
    }

    dx_worker_destroy(&worker);
    return rc;
}
