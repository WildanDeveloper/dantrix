#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "dantrix.h"
#include "fcache.h"
#include "connlimit.h"
#include "fdcount.h"
#include "http_response.h"
#include "net.h"
#include "process.h"
#include "seccomp.h"
#include "status.h"
#include "util.h"
#include "worker.h"

/*
 * Log destinations. The orchestrator owns the FILE streams and the logger
 * structs, and can reopen them in place on SIGUSR1 (logrotate). A worker only
 * borrows the logger pointers, so a reopen never races with logging.
 */
typedef struct {
    dx_log_t log;
    dx_log_t access;
    FILE *log_file;
    FILE *access_file;
    const char *log_path;
    const char *access_path;
    dx_log_level_t level;
} dx_logs_t;

/* Open a log destination: "stdout", "stderr", "off" or a file path. */
static FILE *open_dest(const char *dest)
{
    FILE *f;

    if (dest == NULL || strcmp(dest, "stderr") == 0) {
        return stderr;
    }
    if (strcmp(dest, "stdout") == 0) {
        return stdout;
    }
    if (strcmp(dest, "off") == 0) {
        return NULL;
    }
    f = fopen(dest, "a");
    if (f != NULL) {
        setvbuf(f, NULL, _IOLBF, 0);
        dx_fdcount_opened(DX_FD_LOG);
    }
    return f;
}

/* Close a stream the orchestrator opened (never stdout/stderr). */
static void close_dest(FILE *f)
{
    if (f != NULL && f != stdout && f != stderr) {
        (void)fclose(f);
        dx_fdcount_closed(DX_FD_LOG);
    }
}

/*
 * Reopen both log files. For a path destination the old stream is closed only
 * after the new one is open, so a failed reopen keeps the previous file. The
 * logger's mutex is held only for the pointer swap, never across fopen().
 */
static void logs_reopen(dx_logs_t *logs)
{
    if (logs->log_path != NULL) {
        FILE *nf = open_dest(logs->log_path);

        if (nf != NULL) {
            FILE *old = logs->log_file;

            dx_log_reopen(&logs->log, nf);
            logs->log_file = nf;
            close_dest(old);
        }
    }
    if (logs->access_path != NULL) {
        FILE *nf = open_dest(logs->access_path);

        if (nf != NULL) {
            FILE *old = logs->access_file;

            dx_log_reopen(&logs->access, nf);
            logs->access_file = nf;
            close_dest(old);
        }
    }
}

/* Open both destinations and initialise the loggers. Returns 0 or -1. */
static int logs_open(dx_logs_t *logs, const dx_server_config_t *cfg)
{
    memset(logs, 0, sizeof(*logs));
    logs->log_path = cfg->error_log_path;
    logs->access_path = cfg->access_log_path;
    logs->level = cfg->log_level;

    logs->log_file = open_dest(logs->log_path);
    if (logs->log_file == NULL && strcmp(logs->log_path, "off") != 0) {
        fprintf(stderr, "dantrix: cannot open error log '%s'\n", logs->log_path);
        return -1;
    }
    dx_log_init(&logs->log, logs->log_file, logs->level);
    dx_log_init(&logs->access, NULL, logs->level);

    if (strcmp(logs->access_path, "off") != 0) {
        logs->access_file = open_dest(logs->access_path);
        if (logs->access_file == NULL) {
            fprintf(stderr, "dantrix: cannot open access log '%s'\n", logs->access_path);
            /* The error log is already open: release it, or the failed
             * start-up leaks one fd and one FILE*. */
            close_dest(logs->log_file);
            logs->log_file = NULL;
            return -1;
        }
        dx_log_reopen(&logs->access, logs->access_file);
    }
    return 0;
}

static void logs_close(dx_logs_t *logs)
{
    close_dest(logs->access_file);
    close_dest(logs->log_file);
    dx_log_destroy(&logs->access);
    dx_log_destroy(&logs->log);
    logs->log_file = NULL;
    logs->access_file = NULL;
}

/* Small context so the single-worker reopen hook can reach the log set. */
typedef struct {
    dx_logs_t *logs;
} dx_reopen_ctx_t;

static void single_reopen_hook(void *ctx)
{
    dx_reopen_ctx_t *c = (dx_reopen_ctx_t *)ctx;

    if (c != NULL && c->logs != NULL) {
        logs_reopen(c->logs);
        dx_log_write(&c->logs->log, DX_LOG_INFO, "log files reopened (SIGUSR1)");
    }
}

/* Build the full signal mask handled by signalfd in either mode. */
static void control_signal_set(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGQUIT);
    sigaddset(set, SIGHUP);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
}

/*
 * Log the tuning options the kernel refused, so a configuration that asks for
 * TCP_FASTOPEN on a kernel without it does not silently do nothing. A refusal
 * is a warning, not an error: the listener works without it.
 */
static void warn_refused_tuning(dx_log_t *log, const dx_sockopts_t *refused)
{
    if (refused == NULL) {
        return;
    }
    if (refused->rcvbuf > 0) {
        dx_log_write(log, DX_LOG_WARN, "SO_RCVBUF %zu refused by the kernel: keeping the default",
                     refused->rcvbuf);
    }
    if (refused->sndbuf > 0) {
        dx_log_write(log, DX_LOG_WARN, "SO_SNDBUF %zu refused by the kernel: keeping the default",
                     refused->sndbuf);
    }
    if (refused->defer_accept_ms > 0) {
        dx_log_write(log, DX_LOG_WARN,
                     "TCP_DEFER_ACCEPT %ums refused by the kernel: keeping the default",
                     refused->defer_accept_ms);
    }
    if (refused->fastopen_qlen > 0) {
        dx_log_write(log, DX_LOG_WARN,
                     "TCP_FASTOPEN %u refused (net.ipv4.tcp_fastopen lacks the server bit?): "
                     "keeping the default",
                     refused->fastopen_qlen);
    }
}

/* ------------------------------------------------------------------ */
/* Single-worker path: the main thread runs the event loop              */
/* ------------------------------------------------------------------ */

static int server_run_single(const dx_server_config_t *cfg, dx_logs_t *logs, int ready_fd,
                             dx_status_t *status, uint64_t start_ms, dx_connlimit_t *climit)
{
    dx_worker_t worker;
    dx_worker_opts_t opts;
    dx_worker_reload_t reload;
    dx_reopen_ctx_t reopen_ctx;
    dx_sockopts_t refused;
    sigset_t mask;
    int sig_fd;
    int listen_fd;
    int rc;

    /*
     * Block every control signal process-wide and read them through a
     * signalfd registered in the worker's epoll set: no asynchronous handler
     * runs anywhere, and the loop wakes on epoll like for any other event.
     */
    control_signal_set(&mask);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "pthread_sigmask: %s", dx_strerror(errno));
        return -1;
    }
    sig_fd = dx_signalfd_create(&mask, false);
    if (sig_fd >= 0) {
        dx_fdcount_opened(DX_FD_WAKEUP);
    }
    if (sig_fd < 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "signalfd: %s", dx_strerror(errno));
        return -1;
    }

    listen_fd =
        dx_listen_tcp_ex(cfg->bind_addr, cfg->port, cfg->backlog, false, &cfg->sockopts, &refused);
    if (listen_fd < 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "cannot listen on %s:%d: %s", cfg->bind_addr,
                     cfg->port, dx_strerror(errno));
        (void)close(sig_fd);
        dx_fdcount_closed(DX_FD_WAKEUP);
        return -1;
    }
    warn_refused_tuning(&logs->log, &refused);

    memset(&reload, 0, sizeof(reload));
    pthread_mutex_init(&reload.mu, NULL);
    reload.static_opts = cfg->static_opts;
    reload.life = cfg->life;
    reload.limits = cfg->limits;
    reopen_ctx.logs = logs;

    memset(&opts, 0, sizeof(opts));
    opts.listen_fd = listen_fd;
    opts.ctl_fd = sig_fd;
    opts.ctl_is_signal = true;
    opts.max_conns = cfg->max_conns;
    opts.owns_listen_fd = true;
    opts.log = &logs->log;
    opts.access_log = (cfg->access_log_path[0] != '\0' && strcmp(cfg->access_log_path, "off") != 0)
                          ? &logs->access
                          : NULL;
    opts.log_format = cfg->log_format;
    opts.stop_flag = NULL; /* the signalfd drives the stop */
    opts.limits = &cfg->limits;
    opts.fs = cfg->fs;
    opts.fcache_slots = cfg->fcache_slots;
    opts.fcache_valid_ms = cfg->fcache_valid_ms;
    opts.climit = climit;
    opts.max_conns_per_ip = (cfg->max_conns_per_ip > 0) ? (unsigned)cfg->max_conns_per_ip : 0u;
    opts.mime = cfg->mime;
    opts.static_opts = &cfg->static_opts;
    opts.life = &cfg->life;
    opts.reload = &reload;
    opts.reopen_cb = single_reopen_hook;
    opts.reopen_ctx = &reopen_ctx;
    opts.reload_cb = cfg->reload_cb;
    opts.reload_ctx = cfg->reload_ctx;
    opts.status = status;
    opts.status_path =
        (cfg->status_path != NULL && cfg->status_path[0] != '\0') ? cfg->status_path : NULL;
    opts.start_ms = start_ms;
    opts.total_workers = 1;

    if (dx_worker_init(&worker, &opts) != 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "worker init failed: %s", dx_strerror(errno));
        dx_worker_destroy(&worker);
        (void)close(sig_fd);
        dx_fdcount_closed(DX_FD_WAKEUP);
        pthread_mutex_destroy(&reload.mu);
        return -1;
    }

    dx_log_write(&logs->log, DX_LOG_INFO, "%s %s listening on %s:%d (max %zu connections)",
                 DX_PROGRAM_NAME, DX_VERSION_STRING, cfg->bind_addr, cfg->port, cfg->max_conns);

    /* The listener is bound and the worker is running: the parent may exit. */
    dx_daemon_ready(ready_fd);

    rc = dx_worker_run(&worker);

    if (rc == 0) {
        dx_log_write(&logs->log, DX_LOG_INFO,
                     "shutdown complete (accepted %llu, rejected %llu, closed %llu)",
                     (unsigned long long)worker.accepted_total,
                     (unsigned long long)worker.rejected_total,
                     (unsigned long long)worker.closed_total);
    } else {
        dx_log_write(&logs->log, DX_LOG_ERROR, "worker stopped with an error");
    }

    dx_worker_destroy(&worker);
    (void)close(sig_fd);
    dx_fdcount_closed(DX_FD_WAKEUP);
    pthread_mutex_destroy(&reload.mu);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Multi-worker path (v0.5.0)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    dx_worker_t worker;
    dx_worker_opts_t opts;
    unsigned worker_id;
    int listen_fd; /* the worker's own listener, or the shared one */
    int ctl_fd;    /* borrowed from the orchestrator, never closed here */
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
 * Returns 0, or -1 with errno set. The orchestrator owns every listener: a
 * worker borrows its descriptor and never closes it.
 */
static int setup_listeners(const dx_server_config_t *cfg, dx_logs_t *logs, dx_worker_ctx_t *ctxs,
                           unsigned count, int *shared_fd)
{
    unsigned i;
    dx_sockopts_t refused;

    *shared_fd = -1;
    memset(&refused, 0, sizeof(refused));

    if (cfg->reuseport) {
        for (i = 0; i < count; i++) {
            ctxs[i].listen_fd = dx_listen_tcp_ex(cfg->bind_addr, cfg->port, cfg->backlog, true,
                                                 &cfg->sockopts, &refused);
            if (ctxs[i].listen_fd < 0) {
                break;
            }
            warn_refused_tuning(&logs->log, &refused);
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
                dx_fdcount_closed(DX_FD_LISTEN);
                ctxs[i].listen_fd = -1;
            }
            dx_log_write(&logs->log, DX_LOG_WARN,
                         "SO_REUSEPORT listeners failed (%s), falling back to a shared listener",
                         dx_strerror(saved));
        }
    }

    *shared_fd =
        dx_listen_tcp_ex(cfg->bind_addr, cfg->port, cfg->backlog, false, &cfg->sockopts, &refused);
    if (*shared_fd < 0) {
        return -1;
    }
    warn_refused_tuning(&logs->log, &refused);
    for (i = 0; i < count; i++) {
        ctxs[i].listen_fd = *shared_fd;
        ctxs[i].opts.listen_fd = *shared_fd;
        ctxs[i].opts.owns_listen_fd = false; /* closed once, via shared_fd */
        ctxs[i].opts.exclusive_listen = true;
    }
    return 0;
}

/* Write the aggregated statistics to the error log (after the workers join). */
static void log_stats(dx_logs_t *logs, const dx_worker_ctx_t *ctxs, unsigned count)
{
    dx_worker_stats_t st;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t closed = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        dx_worker_stats(&ctxs[i].worker, &st);
        accepted += st.accepted;
        rejected += st.rejected;
        closed += st.closed;
    }
    dx_log_write(&logs->log, DX_LOG_INFO,
                 "statistics: workers %u, accepted %llu, rejected %llu, closed %llu", count,
                 (unsigned long long)accepted, (unsigned long long)rejected,
                 (unsigned long long)closed);
}

static int server_run_multi(const dx_server_config_t *cfg, dx_logs_t *logs, int ready_fd,
                            dx_status_t *status, uint64_t start_ms, dx_connlimit_t *climit)
{
    const unsigned count = (unsigned)cfg->workers;
    dx_worker_ctx_t *ctxs = NULL;
    pthread_t *threads = NULL;
    dx_worker_reload_t reload;
    sigset_t mask;
    int sig_fd = -1;
    int shared_fd = -1;
    unsigned i;
    unsigned started_count = 0;
    int rc = -1;
    bool stopping = false;

    if (count == 0 || count > 1024u) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The reload snapshot is initialised before the first `goto out`: several
     * early failures (allocation, sigmask, signalfd, bind) jump past the
     * set-up below, and the cleanup runs pthread_mutex_destroy() on the struct
     * unconditionally. Destroying an uninitialised mutex is undefined
     * behaviour, so the mutex must exist from the very first failure path on.
     */
    memset(&reload, 0, sizeof(reload));
    if (pthread_mutex_init(&reload.mu, NULL) != 0) {
        errno = ENOMEM;
        dx_log_write(&logs->log, DX_LOG_ERROR, "cannot initialise the reload mutex");
        return -1;
    }

    ctxs = (dx_worker_ctx_t *)calloc(count, sizeof(*ctxs));
    threads = (pthread_t *)calloc(count, sizeof(*threads));
    if (ctxs == NULL || threads == NULL) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "cannot allocate the worker table");
        goto out;
    }
    /*
     * calloc() zeroes the descriptors to 0, which are valid fd numbers. Mark
     * them unowned before anything can close them.
     */
    for (i = 0; i < count; i++) {
        ctxs[i].worker_id = i;
        ctxs[i].listen_fd = -1;
        ctxs[i].ctl_fd = -1;
        ctxs[i].opts.listen_fd = -1;
        ctxs[i].opts.ctl_fd = -1;
    }

    /*
     * Block the control signals process-wide before the threads exist: every
     * thread inherits the mask, so a signal can never interrupt a worker
     * mid-loop. The main thread alone reads them from its signalfd.
     */
    control_signal_set(&mask);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "pthread_sigmask: %s", dx_strerror(errno));
        goto out;
    }
    sig_fd = dx_signalfd_create(&mask, true);
    if (sig_fd >= 0) {
        dx_fdcount_opened(DX_FD_WAKEUP);
    }
    if (sig_fd < 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "signalfd: %s", dx_strerror(errno));
        goto out;
    }

    /* SIGPIPE is already ignored process-wide (see dx_server_run). */

    if (setup_listeners(cfg, logs, ctxs, count, &shared_fd) != 0) {
        dx_log_write(&logs->log, DX_LOG_ERROR, "cannot listen on %s:%d: %s", cfg->bind_addr,
                     cfg->port, dx_strerror(errno));
        goto out;
    }

    reload.static_opts = cfg->static_opts;
    reload.life = cfg->life;
    reload.limits = cfg->limits;

    /* One control channel per worker, created up front so a stop can always be
     * delivered, even while a worker is still starting up or already gone. */
    for (i = 0; i < count; i++) {
        int wfd = dx_eventfd();

        if (wfd < 0) {
            dx_log_write(&logs->log, DX_LOG_ERROR, "eventfd: %s", dx_strerror(errno));
            goto out;
        }
        dx_fdcount_opened(DX_FD_WAKEUP);
        ctxs[i].ctl_fd = wfd;
        ctxs[i].opts.ctl_fd = wfd;
        ctxs[i].opts.ctl_is_signal = false;
        ctxs[i].opts.worker_id = i;
        ctxs[i].opts.max_conns = cfg->max_conns;
        ctxs[i].opts.log = &logs->log;
        ctxs[i].opts.access_log = (strcmp(cfg->access_log_path, "off") != 0) ? &logs->access : NULL;
        ctxs[i].opts.log_format = cfg->log_format;
        ctxs[i].opts.stop_flag = NULL; /* worker threads stop through the eventfd */
        ctxs[i].opts.limits = &cfg->limits;
        ctxs[i].opts.fs = cfg->fs;
        ctxs[i].opts.fcache_slots = cfg->fcache_slots;
        ctxs[i].opts.fcache_valid_ms = cfg->fcache_valid_ms;
        ctxs[i].opts.climit = climit;
        ctxs[i].opts.max_conns_per_ip =
            (cfg->max_conns_per_ip > 0) ? (unsigned)cfg->max_conns_per_ip : 0u;
        ctxs[i].opts.mime = cfg->mime;
        ctxs[i].opts.static_opts = &cfg->static_opts;
        ctxs[i].opts.life = &cfg->life;
        ctxs[i].opts.reload = &reload;
        ctxs[i].opts.reload_cb = NULL; /* the orchestrator refreshes it */
        ctxs[i].opts.status = status;
        ctxs[i].opts.status_path =
            (cfg->status_path != NULL && cfg->status_path[0] != '\0') ? cfg->status_path : NULL;
        ctxs[i].opts.start_ms = start_ms;
        ctxs[i].opts.total_workers = count;
        ctxs[i].cpu_affinity = cfg->cpu_affinity;
    }

    dx_log_write(&logs->log, DX_LOG_INFO,
                 "%s %s listening on %s:%d (%u workers%s, max %zu connections each)",
                 DX_PROGRAM_NAME, DX_VERSION_STRING, cfg->bind_addr, cfg->port, count,
                 (shared_fd >= 0) ? ", shared listener" : ", SO_REUSEPORT", cfg->max_conns);

    for (i = 0; i < count; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread_main, &ctxs[i]) != 0) {
            dx_log_write(&logs->log, DX_LOG_ERROR, "pthread_create: %s", dx_strerror(errno));
            break;
        }
        ctxs[i].started = true;
        started_count++;
    }

    if (started_count == 0) {
        /* No worker could be created: the process must not hang on the signal. */
        rc = -1;
        goto out;
    }
    if (started_count < count) {
        dx_log_write(&logs->log, DX_LOG_WARN, "only %u of %u workers could be started",
                     started_count, count);
    }

    /* Listeners bound and the workers running: the parent may exit. */
    dx_daemon_ready(ready_fd);

    /*
     * The main thread owns the signalfd and forwards commands to the workers
     * through their eventfds. It never serves requests.
     */
    while (!stopping) {
        struct signalfd_siginfo info;
        unsigned cmd;

        if (read(sig_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
            if (errno == EINTR) {
                continue;
            }
            dx_log_write(&logs->log, DX_LOG_ERROR, "signalfd read: %s", dx_strerror(errno));
            break;
        }

        switch (info.ssi_signo) {
        case SIGTERM:
        case SIGINT:
            dx_log_write(&logs->log, DX_LOG_INFO, "%s received, shutting down %u worker(s)",
                         (info.ssi_signo == SIGINT) ? "SIGINT" : "SIGTERM", count);
            cmd = DX_CMD_SHUTDOWN;
            stopping = true;
            break;
        case SIGQUIT:
            dx_log_write(&logs->log, DX_LOG_INFO, "SIGQUIT received, immediate shutdown");
            cmd = DX_CMD_FORCE;
            stopping = true;
            break;
        case SIGHUP:
            dx_log_write(&logs->log, DX_LOG_INFO, "SIGHUP received, reloading");
            /*
             * Refresh the snapshot first (without the lock: the callback may do
             * file I/O), then fan the RELOAD command out. Each worker copies
             * the snapshot under the mutex, so a partially written snapshot is
             * never visible to them.
             */
            if (cfg->reload_cb != NULL && cfg->reload_cb(cfg->reload_ctx, &reload) != 0) {
                dx_log_write(&logs->log, DX_LOG_ERROR,
                             "reload failed, keeping the previous settings");
                continue;
            }
            cmd = DX_CMD_RELOAD;
            break;
        case SIGUSR1:
            logs_reopen(logs);
            dx_log_write(&logs->log, DX_LOG_INFO, "log files reopened (SIGUSR1)");
            continue; /* nothing to forward: only the orchestrator owns streams */
        case SIGUSR2:
            /*
             * Every worker logs its own counters from its own thread. Reading
             * them from here while the workers run would be a data race.
             */
            cmd = DX_CMD_STATS;
            break;
        default:
            continue;
        }

        /*
         * The eventfd payload IS the command bitmask. Writing 1 first (the
         * generic "signal a wakeup" helper) would also set the SHUTDOWN bit,
         * so the command value is written directly, once per worker.
         */
        for (i = 0; i < count; i++) {
            if (ctxs[i].ctl_fd >= 0) {
                uint64_t value = cmd;
                ssize_t written = write(ctxs[i].ctl_fd, &value, sizeof(value));

                (void)written; /* a failing fd is reported at join time */
            }
        }
    }

    rc = 0;
    for (i = 0; i < count; i++) {
        if (ctxs[i].started && pthread_join(threads[i], NULL) != 0) {
            rc = -1;
        }
    }

    for (i = 0; i < count; i++) {
        if (ctxs[i].started && (ctxs[i].init_rc != 0 || ctxs[i].run_rc != 0)) {
            dx_log_write(&logs->log, DX_LOG_ERROR, "worker %u stopped with an error", i);
            rc = -1;
        }
    }
    if (rc == 0) {
        log_stats(logs, ctxs, count);
    }

out:
    /*
     * The orchestrator owns exactly what no thread could own: the shared
     * listener, every reuseport listener (workers borrow theirs), every control
     * eventfd and the signalfd.
     */
    if (ctxs != NULL && shared_fd < 0) {
        for (i = 0; i < count; i++) {
            if (ctxs[i].listen_fd >= 0) {
                (void)close(ctxs[i].listen_fd);
                dx_fdcount_closed(DX_FD_LISTEN);
                ctxs[i].listen_fd = -1;
            }
        }
    }
    if (shared_fd >= 0) {
        (void)close(shared_fd);
        dx_fdcount_closed(DX_FD_LISTEN);
    }
    if (ctxs != NULL) {
        for (i = 0; i < count; i++) {
            if (ctxs[i].ctl_fd >= 0) {
                (void)close(ctxs[i].ctl_fd);
                dx_fdcount_closed(DX_FD_WAKEUP);
            }
        }
    }
    if (sig_fd >= 0) {
        (void)close(sig_fd);
        dx_fdcount_closed(DX_FD_WAKEUP);
    }
    pthread_mutex_destroy(&reload.mu);
    free(threads);
    free(ctxs);
    return rc;
}

int dx_server_run(const dx_server_config_t *cfg)
{
    dx_logs_t logs;
    dx_status_t status;
    dx_connlimit_t *climit;
    uint64_t start_ms = (uint64_t)dx_now_ms();
    int rc;
    int ready_fd = -1;
    bool wrote_pid = false;

    dx_status_init(&status);
    /*
     * Point the response renderer at the operator's extra headers, once, before
     * any worker exists. Doing it here rather than threading a parameter through
     * every render call is what makes it impossible for a response path to
     * forget them.
     */
    if (cfg != NULL) {
        dx_http_response_set_extra_headers(cfg->static_opts.add_header,
                                           cfg->static_opts.add_header_count);
    }
    /*
     * One table for the whole server, shared by every worker: the limit has to
     * count the client, not one worker's share of it. NULL when the feature is
     * off, and every connlimit call treats NULL as "allow".
     */
    climit = (cfg != NULL && cfg->max_conns_per_ip > 0) ? dx_connlimit_create(cfg->connlimit_slots)
                                                        : NULL;

    /*
     * Both log destinations are dereferenced unconditionally in the paths
     * below (strcmp against "off" and "stderr"), so neither may be NULL. The
     * error log is a hard requirement, the access log defaults to "off" when
     * it is omitted.
     */
    if (cfg == NULL || cfg->error_log_path == NULL || cfg->access_log_path == NULL ||
        cfg->port < 1 || cfg->port > 65535) {
        errno = EINVAL;
        return -1;
    }

    /*
     * SIGPIPE must be ignored process-wide, in every mode, before anything can
     * write to a socket or a stdio stream: sendfile() has no MSG_NOSIGNAL, and
     * a log destination whose reader has exited (a pipe fed to `head`, for
     * example) kills the whole server on the next log line. This used to be
     * installed only in server_run_multi(), so single-worker mode -- the
     * default -- was unprotected.
     */
    {
        struct sigaction ign;
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        if (sigaction(SIGPIPE, &ign, NULL) != 0) {
            fprintf(stderr, "dantrix: sigaction(SIGPIPE): %s\n", dx_strerror(errno));
            return -1;
        }
    }

    /*
     * Daemonize first: the parent blocks until the child reports readiness, so
     * a start-up failure (bind, bad root) is still reported with exit code 1
     * instead of silently returning success.
     */
    if (cfg->daemonize) {
        int drc = dx_daemonize(&ready_fd);

        if (drc < 0) {
            fprintf(stderr, "dantrix: cannot daemonize: %s\n", dx_strerror(errno));
            return -1;
        }
        if (drc == 1) {
            dx_connlimit_destroy(climit);
            return 0; /* parent: the child owns the server now */
        }
    }

    if (logs_open(&logs, cfg) != 0) {
        dx_daemon_fail(ready_fd);
        dx_connlimit_destroy(climit);
        return -1;
    }

    if (cfg->pid_file != NULL && cfg->pid_file[0] != '\0') {
        if (dx_pid_file_write(cfg->pid_file) != 0) {
            dx_log_write(&logs.log, DX_LOG_ERROR, "cannot write pid file '%s': %s", cfg->pid_file,
                         dx_strerror(errno));
            logs_close(&logs);
            dx_daemon_fail(ready_fd);
            dx_connlimit_destroy(climit);
            return -1;
        }
        wrote_pid = true;
    }

    /*
     * Drop privileges before the first request is served. The document root
     * and the log files were opened by the (possibly privileged) parent, and
     * bind() happens inside the worker setup: dropping here means a port below
     * 1024 still binds, and every request afterwards runs unprivileged.
     */
    if (dx_drop_privileges(cfg->user, cfg->group, &logs.log) != 0) {
        if (wrote_pid) {
            dx_pid_file_remove(cfg->pid_file);
        }
        logs_close(&logs);
        dx_daemon_fail(ready_fd);
        dx_connlimit_destroy(climit);
        return -1;
    }

    /*
     * The sandbox goes on last, after the listener exists, the log files are
     * open and privileges have been dropped: everything the filter allows has
     * already happened, and every thread created from here inherits it. A
     * kernel that cannot install it is a warning, not a failure -- the server
     * is still able to serve without the extra guarantee.
     */
    if (cfg->seccomp) {
        if (dx_seccomp_apply() != 0) {
            dx_log_write(&logs.log, DX_LOG_WARN,
                         "seccomp filter not installed (%s); continuing without the sandbox",
                         dx_strerror(errno));
        } else {
            size_t n = 0;

            (void)dx_seccomp_allowlist(&n);
            dx_log_write(&logs.log, DX_LOG_INFO,
                         "seccomp filter active: %zu syscalls allowed, everything else kills the "
                         "process",
                         n);
        }
    }

    if (cfg->workers > 1) {
        rc = server_run_multi(cfg, &logs, ready_fd, &status, start_ms, climit);
    } else {
        rc = server_run_single(cfg, &logs, ready_fd, &status, start_ms, climit);
    }

    if (wrote_pid) {
        dx_pid_file_remove(cfg->pid_file);
    }
    logs_close(&logs);
    dx_connlimit_destroy(climit);
    return rc;
}
