#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dantrix.h"
#include "net.h"
#include "util.h"
#include "worker.h"

/*
 * The flag is written from a signal handler and read from the event loop. Only
 * sig_atomic_t writes of 0/1 are used, which is the exact contract the C
 * standard guarantees for this pattern. It is only used by the single-worker
 * path, where handler and loop share one thread.
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

/* Install the v0.1 signal set for the single-worker path. Returns 0 or -1. */
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

/* ------------------------------------------------------------------ */
/* Single-worker path (unchanged since v0.1): main thread runs the loop */
/* ------------------------------------------------------------------ */

static int server_run_single(const dx_server_config_t *cfg)
{
    dx_worker_t worker;
    dx_worker_opts_t opts;
    int listen_fd;
    int rc = 0;

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
    memset(&opts, 0, sizeof(opts));
    opts.listen_fd = listen_fd;
    opts.wake_fd = -1;
    opts.max_conns = cfg->max_conns;
    opts.owns_listen_fd = true;
    opts.log = cfg->log;
    opts.access_log = cfg->access_log;
    opts.log_format = cfg->log_format;
    opts.stop_flag = &g_stop;
    opts.limits = &cfg->limits;
    opts.fs = cfg->fs;
    opts.mime = cfg->mime;
    opts.static_opts = &cfg->static_opts;
    opts.life = &cfg->life;

    if (dx_worker_init(&worker, &opts) != 0) {
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

/* ------------------------------------------------------------------ */
/* Multi-worker path (v0.5.0)                                          */
/* ------------------------------------------------------------------ */

/*
 * Per-thread context. The worker struct lives in a server-owned array: the
 * main thread needs it after join() to aggregate the statistics, so it cannot
 * be a local of the thread function.
 */
typedef struct {
    dx_worker_t worker;
    dx_worker_opts_t opts;
    unsigned worker_id;
    int listen_fd; /* the worker's own listener, or the shared one */
    int wake_fd;   /* borrowed from the orchestrator, never closed here */
    bool cpu_affinity;
    bool started; /* pthread_create() succeeded */
    int init_rc;
    int run_rc;
} dx_worker_ctx_t;

static void *worker_thread_main(void *arg)
{
    dx_worker_ctx_t *ctx = (dx_worker_ctx_t *)arg;

    ctx->init_rc = dx_worker_init(&ctx->worker, &ctx->opts);
    if (ctx->init_rc == 0) {
        if (ctx->cpu_affinity && dx_set_affinity(ctx->worker_id) != 0) {
            /* Affinity is a performance hint, never a fatal error. */
            dx_log_write(ctx->opts.log, DX_LOG_WARN, "worker %u: cannot pin to a CPU: %s",
                         ctx->worker_id, dx_strerror(errno));
        }
        ctx->run_rc = dx_worker_run(&ctx->worker);
    }
    dx_worker_destroy(&ctx->worker);
    return NULL;
}

/*
 * Create the worker group listeners.
 *
 * reuseport mode: one listener per worker, all bound to the same address with
 * SO_REUSEPORT; the kernel load-balances new connections across them.
 *
 * shared mode (fallback, also selected by --no-reuseport): one listener
 * registered in every worker's epoll with EPOLLEXCLUSIVE so a new connection
 * wakes exactly one worker.
 *
 * Returns 0, or -1 with errno set. On success *shared_fd is -1 in reuseport
 * mode (each ctxs[i].listen_fd is a distinct, valid descriptor) or the single
 * shared descriptor in shared mode. The orchestrator owns every listener: a
 * worker borrows its descriptor and never closes it.
 */
static int setup_listeners(const dx_server_config_t *cfg, dx_worker_ctx_t *ctxs, unsigned count,
                           int *shared_fd)
{
    unsigned i;

    *shared_fd = -1;

    if (cfg->reuseport) {
        for (i = 0; i < count; i++) {
            ctxs[i].listen_fd = dx_listen_tcp_reuseport(cfg->bind_addr, cfg->port, cfg->backlog);
            if (ctxs[i].listen_fd < 0) {
                break;
            }
        }
        if (i == count) {
            for (i = 0; i < count; i++) {
                ctxs[i].opts.listen_fd = ctxs[i].listen_fd;
                ctxs[i].opts.owns_listen_fd = false; /* the orchestrator closes it */
                ctxs[i].opts.exclusive_listen = false;
            }
            return 0;
        }

        /*
         * SO_REUSEPORT unavailable (old kernel) or a later bind failed: roll
         * the listeners that were already created back and fall through to the
         * shared mode. errno is saved first: close() may overwrite it and the
         * warning below must report the real bind failure.
         */
        {
            int saved = errno;

            while (i > 0) {
                i--;
                (void)close(ctxs[i].listen_fd);
                ctxs[i].listen_fd = -1;
            }
            dx_log_write(cfg->log, DX_LOG_WARN,
                         "SO_REUSEPORT listeners failed (%s), falling back to a shared listener",
                         dx_strerror(saved));
        }
    }

    *shared_fd = dx_listen_tcp(cfg->bind_addr, cfg->port, cfg->backlog);
    if (*shared_fd < 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        ctxs[i].listen_fd = *shared_fd;
        ctxs[i].opts.listen_fd = *shared_fd;
        ctxs[i].opts.owns_listen_fd = false; /* closed once, via shared_fd */
        ctxs[i].opts.exclusive_listen = true;
    }
    return 0;
}

static int server_run_multi(const dx_server_config_t *cfg)
{
    const unsigned count = (unsigned)cfg->workers;
    dx_worker_ctx_t *ctxs = NULL;
    pthread_t *threads = NULL;
    int shared_fd = -1;
    unsigned i;
    sigset_t stop_sigs;
    sigset_t old_mask;
    bool mask_changed = false;
    int rc = -1;

    if (count == 0 || count > 1024u) {
        errno = EINVAL;
        return -1;
    }

    ctxs = (dx_worker_ctx_t *)calloc(count, sizeof(*ctxs));
    threads = (pthread_t *)calloc(count, sizeof(*threads));
    if (ctxs == NULL || threads == NULL) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "cannot allocate the worker table");
        goto out;
    }
    /*
     * calloc() zeroes the descriptors to 0, which are valid fd numbers. Mark
     * them unowned before anything can close them.
     */
    for (i = 0; i < count; i++) {
        ctxs[i].worker_id = i;
        ctxs[i].listen_fd = -1;
        ctxs[i].wake_fd = -1;
        ctxs[i].opts.listen_fd = -1;
        ctxs[i].opts.wake_fd = -1;
    }

    /*
     * Block the stop signals process-wide before the threads exist: every
     * thread inherits the mask, so the signal can never interrupt a worker
     * mid-loop. The main thread alone catches them with sigwait(), which
     * removes the signal-handler-to-thread delivery problem entirely.
     */
    sigemptyset(&stop_sigs);
    sigaddset(&stop_sigs, SIGINT);
    sigaddset(&stop_sigs, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stop_sigs, &old_mask) != 0) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "pthread_sigmask: %s", dx_strerror(errno));
        goto out;
    }
    mask_changed = true;

    /* sendfile() has no MSG_NOSIGNAL, so SIGPIPE must be ignored process-wide. */
    {
        struct sigaction ign;
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        if (sigaction(SIGPIPE, &ign, NULL) != 0) {
            dx_log_write(cfg->log, DX_LOG_ERROR, "sigaction(SIGPIPE): %s", dx_strerror(errno));
            goto out;
        }
    }

    if (setup_listeners(cfg, ctxs, count, &shared_fd) != 0) {
        dx_log_write(cfg->log, DX_LOG_ERROR, "cannot listen on %s:%d: %s", cfg->bind_addr,
                     cfg->port, dx_strerror(errno));
        goto out;
    }

    /* One wakeup channel per worker, created up front so a stop can always be
     * delivered, even while a worker is still starting up or already gone. */
    for (i = 0; i < count; i++) {
        int wfd = dx_eventfd();

        if (wfd < 0) {
            dx_log_write(cfg->log, DX_LOG_ERROR, "eventfd: %s", dx_strerror(errno));
            goto out;
        }
        ctxs[i].wake_fd = wfd;
        ctxs[i].opts.wake_fd = wfd;
        ctxs[i].opts.worker_id = i;
        ctxs[i].opts.max_conns = cfg->max_conns;
        ctxs[i].opts.log = cfg->log;
        ctxs[i].opts.access_log = cfg->access_log;
        ctxs[i].opts.log_format = cfg->log_format;
        ctxs[i].opts.stop_flag = NULL; /* worker threads stop through the wake_fd */
        ctxs[i].opts.limits = &cfg->limits;
        ctxs[i].opts.fs = cfg->fs;
        ctxs[i].opts.mime = cfg->mime;
        ctxs[i].opts.static_opts = &cfg->static_opts;
        ctxs[i].opts.life = &cfg->life;
        ctxs[i].cpu_affinity = cfg->cpu_affinity;
    }

    dx_log_write(cfg->log, DX_LOG_INFO,
                 "%s %s listening on %s:%d (%u workers%s, max %zu connections each)",
                 DX_PROGRAM_NAME, DX_VERSION_STRING, cfg->bind_addr, cfg->port, count,
                 (shared_fd >= 0) ? ", shared listener" : ", SO_REUSEPORT", cfg->max_conns);

    unsigned started_count = 0;
    for (i = 0; i < count; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread_main, &ctxs[i]) != 0) {
            dx_log_write(cfg->log, DX_LOG_ERROR, "pthread_create: %s", dx_strerror(errno));
            break;
        }
        ctxs[i].started = true;
        started_count++;
    }

    if (started_count == 0) {
        /* No worker could be created: the process must not hang on sigwait. */
        rc = -1;
        goto out;
    }
    if (started_count < count) {
        dx_log_write(cfg->log, DX_LOG_WARN, "only %u of %u workers could be started", started_count,
                     count);
    }

    /* The main thread waits for the stop signal; it never serves requests. */
    {
        int sig = 0;
        int swrc;

        do {
            swrc = sigwait(&stop_sigs, &sig);
        } while (swrc == EINTR);
        if (swrc == 0) {
            dx_log_write(cfg->log, DX_LOG_INFO, "%s received, shutting down %u worker(s)",
                         (sig == SIGINT) ? "SIGINT" : "SIGTERM", count);
        } else {
            dx_log_write(cfg->log, DX_LOG_ERROR, "sigwait: %s", dx_strerror(swrc));
        }
    }

    for (i = 0; i < count; i++) {
        (void)dx_eventfd_signal(ctxs[i].wake_fd);
    }

    rc = 0;
    for (i = 0; i < count; i++) {
        if (ctxs[i].started && pthread_join(threads[i], NULL) != 0) {
            rc = -1;
        }
    }

    {
        dx_worker_stats_t st;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
        uint64_t closed = 0;

        for (i = 0; i < count; i++) {
            dx_worker_stats(&ctxs[i].worker, &st);
            accepted += st.accepted;
            rejected += st.rejected;
            closed += st.closed;
            if (ctxs[i].started && (ctxs[i].init_rc != 0 || ctxs[i].run_rc != 0)) {
                dx_log_write(cfg->log, DX_LOG_ERROR, "worker %u stopped with an error", i);
                rc = -1;
            }
        }
        if (rc == 0) {
            dx_log_write(cfg->log, DX_LOG_INFO,
                         "shutdown complete (accepted %llu, rejected %llu, closed %llu)",
                         (unsigned long long)accepted, (unsigned long long)rejected,
                         (unsigned long long)closed);
        }
    }

out:
    /*
     * The orchestrator owns exactly what no thread could own: the shared
     * listener, every wake fd, and every reuseport listener (workers borrow
     * theirs). Joined workers destroyed their own state inside the thread.
     */
    if (ctxs != NULL && shared_fd < 0) {
        for (i = 0; i < count; i++) {
            if (ctxs[i].listen_fd >= 0) {
                (void)close(ctxs[i].listen_fd);
                ctxs[i].listen_fd = -1;
            }
        }
    }
    if (shared_fd >= 0) {
        (void)close(shared_fd);
    }
    if (ctxs != NULL) {
        for (i = 0; i < count; i++) {
            if (ctxs[i].wake_fd >= 0) {
                (void)close(ctxs[i].wake_fd);
            }
        }
    }
    free(threads);
    free(ctxs);
    if (mask_changed) {
        (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    }
    return rc;
}

int dx_server_run(const dx_server_config_t *cfg)
{
    if (cfg == NULL || cfg->log == NULL || cfg->port < 1 || cfg->port > 65535) {
        errno = EINVAL;
        return -1;
    }

    if (cfg->workers > 1) {
        return server_run_multi(cfg);
    }
    return server_run_single(cfg);
}
