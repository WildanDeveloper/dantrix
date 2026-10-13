#include "worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dantrix.h"
#include "http_response.h"
#include "net.h"
#include "status.h"
#include "util.h"

#include <time.h>

dx_log_t *dx_worker_log(dx_worker_t *w)
{
    return (w == NULL) ? NULL : w->log;
}

static int listener_arm(dx_worker_t *w);
static int ctl_arm(dx_worker_t *w);

int dx_worker_init(dx_worker_t *w, const dx_worker_opts_t *opts)
{
    if (w == NULL) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Zero and neutralise the descriptors before any validation: a caller that
     * runs dx_worker_destroy() after a failed init must never close fd 0, which
     * a plain calloc()/stack slot would present as an "open" descriptor.
     */
    memset(w, 0, sizeof(*w));
    w->epfd = -1;
    w->listen_fd = -1;
    w->ctl_fd = -1;

    if (opts == NULL || opts->listen_fd < 0 || opts->max_conns == 0) {
        errno = EINVAL;
        return -1;
    }

    w->listen_fd = opts->listen_fd;
    w->ctl_fd = (opts->ctl_fd >= 0) ? opts->ctl_fd : -1;
    w->ctl_is_signal = opts->ctl_is_signal;
    w->worker_id = opts->worker_id;
    w->owns_listen_fd = opts->owns_listen_fd;
    w->exclusive_listen = opts->exclusive_listen;
    w->reload = opts->reload;
    w->reopen_cb = opts->reopen_cb;
    w->reopen_ctx = opts->reopen_ctx;
    w->reload_cb = opts->reload_cb;
    w->reload_ctx = opts->reload_ctx;
    w->status = opts->status;
    w->status_path = opts->status_path;
    w->start_ms = (opts->start_ms != 0) ? opts->start_ms : (uint64_t)dx_now_ms();
    w->total_workers = (opts->total_workers != 0) ? opts->total_workers : 1u;
    dx_timer_node_reset(&w->accept_timer);
    w->log = opts->log;
    w->access_log = opts->access_log;
    w->log_format = opts->log_format;
    w->fs = opts->fs;
    w->mime = opts->mime;
    if (opts->limits != NULL) {
        w->limits = *opts->limits;
    } else {
        dx_http_limits_default(&w->limits);
    }
    if (opts->static_opts != NULL) {
        w->static_opts = *opts->static_opts;
    }
    if (opts->life != NULL) {
        w->life = *opts->life;
    }
    if (w->static_opts.cache_control == NULL) {
        w->static_opts.cache_control = "";
    }
    if (w->static_opts.sendfile_chunk == 0) {
        w->static_opts.sendfile_chunk = 1024u * 1024u;
    }
    if (w->life.keepalive_requests == 0) {
        w->life.keepalive_requests = 1000u;
    }
    w->max_conns = opts->max_conns;
    w->stop_flag = opts->stop_flag;

    w->pool = dx_pool_create(DX_CONN_CHUNK_SLOTS, opts->max_conns);
    if (w->pool == NULL) {
        errno = ENOMEM;
        return -1;
    }
    w->bufs = dx_bufpool_create(DX_CONN_BUF_SIZE, DX_BUF_CHUNK_SLOTS);
    if (w->bufs == NULL) {
        errno = ENOMEM;
        return -1;
    }
    /*
     * The HTTP header table is pooled separately from the read buffer: it used
     * to be an inline array inside every connection, which cost 8 KiB per
     * connection even while it sat idle between keep-alive requests. Pooling it
     * means an idle connection costs the slot alone and the table is only held
     * while a request is actually being parsed.
     */
    w->hdrs =
        dx_bufpool_create(DX_MAX_HEADERS_LIMIT * sizeof(dx_http_header_t), DX_BUF_CHUNK_SLOTS);
    if (w->hdrs == NULL) {
        dx_bufpool_destroy(w->bufs);
        w->bufs = NULL;
        errno = ENOMEM;
        return -1;
    }
    w->timers = dx_timer_wheel_create();
    if (w->timers == NULL) {
        errno = ENOMEM;
        return -1;
    }

    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (w->epfd < 0) {
        return -1;
    }

    if (ctl_arm(w) != 0) {
        return -1;
    }
    if (listener_arm(w) != 0) {
        return -1;
    }
    return 0;
}

void dx_worker_destroy(dx_worker_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w->epfd >= 0) {
        (void)close(w->epfd);
        w->epfd = -1;
    }
    /*
     * Only a worker that owns its listener closes it. In the shared-listener
     * fallback the descriptor belongs to the orchestrator and is closed once,
     * after every worker has joined.
     */
    if (w->owns_listen_fd && w->listen_fd >= 0) {
        (void)close(w->listen_fd);
    }
    w->listen_fd = -1;
    dx_timer_wheel_cancel(w->timers, &w->accept_timer);
    dx_pool_destroy(w->pool);
    w->pool = NULL;
    dx_bufpool_destroy(w->bufs);
    w->bufs = NULL;
    dx_bufpool_destroy(w->hdrs);
    w->hdrs = NULL;
    dx_timer_wheel_destroy(w->timers);
    w->timers = NULL;
}

void dx_worker_stats(const dx_worker_t *w, dx_worker_stats_t *out)
{
    if (w == NULL || out == NULL) {
        return;
    }
    out->accepted = w->accepted_total;
    out->rejected = w->rejected_total;
    out->closed = w->closed_total;
}

/*
 * Refresh the shared snapshot from the configuration sources, then copy it
 * into this worker's own settings. Both steps happen under the same mutex, so
 * a partially written snapshot is never visible.
 *
 * The hot path never touches the snapshot: it only reads this worker's copy,
 * so a reload costs one lock per worker and nothing per request.
 */
void dx_worker_apply_reload(dx_worker_t *w)
{
    if (w == NULL || w->reload == NULL) {
        return;
    }
    if (w->reload_cb != NULL && w->reload_cb(w->reload_ctx, w->reload) != 0) {
        dx_log_write(w->log, DX_LOG_ERROR, "reload failed, keeping the previous settings");
        return;
    }
    pthread_mutex_lock(&w->reload->mu);
    w->static_opts = w->reload->static_opts;
    w->life = w->reload->life;
    w->limits = w->reload->limits;
    pthread_mutex_unlock(&w->reload->mu);

    if (w->static_opts.cache_control == NULL) {
        w->static_opts.cache_control = "";
    }
    if (w->static_opts.sendfile_chunk == 0) {
        w->static_opts.sendfile_chunk = 1024u * 1024u;
    }
    if (w->life.keepalive_requests == 0) {
        w->life.keepalive_requests = 1000u;
    }
    dx_log_write(w->log, DX_LOG_INFO, "worker %u: reloadable settings applied", w->worker_id);
}

/* Format the peer address for the access log; "-" when unknown. */
static void format_peer(const struct sockaddr_storage *ss, char *out, size_t cap)
{
    const void *src;
    int family;

    if (ss == NULL || out == NULL || cap == 0) {
        return;
    }
    family = ss->ss_family;
    if (family == AF_INET) {
        src = &((const struct sockaddr_in *)(const void *)ss)->sin_addr;
    } else if (family == AF_INET6) {
        src = &((const struct sockaddr_in6 *)(const void *)ss)->sin6_addr;
    } else {
        out[0] = '-';
        out[1] = '\0';
        return;
    }
    if (inet_ntop(family, src, out, (socklen_t)cap) == NULL) {
        out[0] = '-';
        out[1] = '\0';
    }
}

/* Register a new connection. Returns 0, or -1 with *fd consumed on failure. */
static int conn_open(dx_worker_t *w, int fd, const struct sockaddr_storage *peer)
{
    unsigned char *mem;
    dx_conn_t *conn;
    struct epoll_event ev;

    mem = (unsigned char *)dx_bufpool_acquire(w->bufs, DX_CONN_BUF_SIZE);
    if (mem == NULL) {
        (void)close(fd);
        dx_log_write(w->log, DX_LOG_WARN,
                     "connection rejected: read buffer pool exhausted "
                     "(%zu buffers in use)",
                     dx_bufpool_outstanding(w->bufs));
        return -1;
    }

    conn = dx_pool_acquire(w->pool);
    if (conn == NULL) {
        (void)dx_bufpool_release(w->bufs, mem);
        (void)close(fd);
        dx_log_write(w->log, DX_LOG_WARN, "connection rejected: limit of %zu connections reached",
                     w->max_conns);
        return -1;
    }

    dx_conn_init(conn, fd, &w->limits);
    conn->buf_mem = mem;
    dx_buffer_init(&conn->buf, mem, DX_CONN_BUF_SIZE);
    format_peer(peer, conn->peer, sizeof(conn->peer));
    /* The header clock starts at accept, so a silent client cannot hold a
     * connection slot forever. */
    dx_conn_arm_timer(w, conn);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.u64 = dx_tag_conn(conn->index, conn->generation);
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        int saved = errno;

        /*
         * The timer was armed before epoll_ctl. Cancelling it here keeps the
         * dead node out of its bucket (on_timer_expired would ignore it, but
         * the node would still occupy the slot until it fired).
         */
        dx_timer_wheel_cancel(w->timers, &conn->timer);
        (void)close(fd);
        /* Buffer first: the slot must be given back while it still knows
         * whether its memory came from the pool. */
        (void)dx_bufpool_release(w->bufs, mem);
        (void)dx_pool_release(w->pool, conn);
        dx_log_write(w->log, DX_LOG_ERROR, "epoll_ctl(ADD): %s", dx_strerror(saved));
        errno = saved;
        return -1;
    }

    /* TCP_NODELAY only changes latency; a failure must not drop the client. */
    (void)dx_set_tcp_nodelay(fd);
    w->accepted_total++;
    dx_status_conn_accepted(w->status);
    return 0;
}

/*
 * Emit the access log line for the request that was served, if any.
 *
 * Called once per completed request: from conn_after_response() for the normal
 * path, and from conn_close() for a request that never reached a response
 * (a 408 written on a timeout, or a connection dropped mid-request). The
 * record's status is cleared after it is written, so a request is logged
 * exactly once no matter which path runs last.
 */
void dx_worker_emit_access(dx_worker_t *w, dx_conn_t *conn)
{
    const dx_access_t *acc = &conn->acc;

    if (acc->status == 0) {
        return; /* the request never reached a response */
    }
    if (w->access_log != NULL) {
        dx_log_access(w->access_log, w->log_format, conn->peer, acc->method, strlen(acc->method),
                      acc->target, strlen(acc->target), acc->http10 ? "HTTP/1.0" : "HTTP/1.1",
                      acc->status, acc->body_bytes, acc->referer, acc->user_agent);
    }
    /*
     * The shared counters are updated on the same "response completed" edge as
     * the access log, independently of whether a log destination is set.
     */
    dx_status_response(w->status, acc->status, (uint64_t)acc->body_bytes);
    conn->acc.status = 0; /* one line per request, whichever path runs first */
}

/* Tear a connection down and give its slot and buffer back to the pools. */
static void conn_close(dx_worker_t *w, dx_conn_t *conn)
{
    int fd;

    if (conn == NULL || conn->state == DX_CONN_FREE) {
        return;
    }
    conn->state = DX_CONN_CLOSING;
    dx_timer_wheel_cancel(w->timers, &conn->timer);

    fd = conn->fd;
    conn->fd = -1;

    if (w->epfd >= 0 && fd >= 0) {
        (void)epoll_ctl(w->epfd, EPOLL_CTL_DEL, fd, NULL);
    }
    if (fd >= 0) {
        (void)close(fd);
    }

    /* Any request that never reached a normal response is logged here. */
    dx_worker_emit_access(w, conn);

    /* A streamed file is owned by the connection until it is closed. */
    dx_conn_release_file(conn);
    /* Pipelined bytes parked for the next request. */
    dx_conn_release_hold(conn);

    /* Returns the pooled slot or frees the grown heap allocation. */
    dx_conn_release_buffer(conn, w->bufs);
    dx_conn_release_headers(conn, w->hdrs);
    /* dx_pool_release() resets the slot, so nothing survives this point. */
    (void)dx_pool_release(w->pool, conn);
    w->closed_total++;
    dx_status_conn_closed(w->status);
}

/*
 * Keep the registered interest in sync with the connection state. EPOLLOUT is
 * only registered while output is pending: on a level-triggered epoll it would
 * otherwise keep the loop spinning. After an EOF, EPOLLIN is dropped for the
 * same reason: read() would return 0 forever.
 */
static int conn_update_interest(dx_worker_t *w, dx_conn_t *conn)
{
    struct epoll_event ev;

    if (w->epfd < 0 || conn->fd < 0) {
        return -1;
    }
    memset(&ev, 0, sizeof(ev));
    if (conn->eof || dx_conn_has_work(conn)) {
        /*
         * Only EPOLLOUT while output is pending: on a level-triggered epoll
         * it would otherwise keep the loop spinning. After an EOF read()
         * would return 0 forever, so EPOLLIN is dropped as well.
         */
        if (!dx_conn_has_work(conn)) {
            return -1; /* nothing left to send: the caller closes */
        }
        ev.events = EPOLLOUT;
    } else {
        ev.events = EPOLLIN | EPOLLRDHUP;
    }
    ev.data.u64 = dx_tag_conn(conn->index, conn->generation);
    return epoll_ctl(w->epfd, EPOLL_CTL_MOD, conn->fd, &ev);
}

/* Delay before the accept queue is retried after a resource failure. */
#define DX_ACCEPT_BACKOFF_MS 100u

/* Add (or re-add) the listener to the epoll set. Returns 0 or -1. */
static int listener_arm(dx_worker_t *w)
{
    struct epoll_event ev;
    uint32_t events = EPOLLIN;

    if (w->epfd < 0 || w->listen_fd < 0) {
        return -1;
    }
    if (w->exclusive_listen) {
        /*
         * Shared-listener mode: without this flag every new connection wakes
         * the whole worker group (thundering herd).
         */
        events |= EPOLLEXCLUSIVE;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u64 = dx_tag_listener();
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &ev) == 0) {
        return 0;
    }
    if (w->exclusive_listen && errno == EINVAL) {
        /* Kernel too old for EPOLLEXCLUSIVE: accept without it. */
        w->exclusive_listen = false;
        ev.events = EPOLLIN;
        return epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &ev);
    }
    return -1;
}

/* Register the borrowed control fd in the epoll set. Returns 0 or -1. */
static int ctl_arm(dx_worker_t *w)
{
    struct epoll_event ev;

    if (w->ctl_fd < 0) {
        return 0; /* no control channel: stop only through the signal flag */
    }
    if (w->epfd < 0) {
        return -1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = dx_tag_wake();
    return epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->ctl_fd, &ev);
}

/*
 * Descriptor exhaustion: stop accepting without blocking the loop. The
 * listener leaves the epoll set (otherwise EPOLLIN refires at once and spins
 * the loop on a queue it cannot drain) and a timer wheel node brings it back.
 */
static void accept_backoff(dx_worker_t *w)
{
    if (w->epfd >= 0 && w->listen_fd >= 0) {
        (void)epoll_ctl(w->epfd, EPOLL_CTL_DEL, w->listen_fd, NULL);
    }
    w->accept_timer.owner = w;
    (void)dx_timer_wheel_refresh(w->timers, &w->accept_timer, DX_ACCEPT_BACKOFF_MS,
                                 (uint64_t)dx_now_ms());
}

/* Accept until the queue is empty. Never blocks: the socket is non-blocking. */
static void accept_loop(dx_worker_t *w)
{
    for (;;) {
        /* Zeroed so format_peer() never reads padding of an address accept4()
         * did not fill in (it can report a short address length). */
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int fd;

        memset(&peer, 0, sizeof(peer));
        fd = accept4(w->listen_fd, (struct sockaddr *)&peer, &peer_len,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd >= 0) {
            if (conn_open(w, fd, &peer) != 0) {
                /*
                 * conn_open consumed the descriptor on every failure path, so
                 * there is nothing left to close here.
                 */
                w->rejected_total++;
                dx_status_conn_rejected(w->status);
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* queue drained */
        }
        if (errno == ECONNABORTED || errno == EPROTO) {
            /* The peer went away during the handshake: not an error. */
            continue;
        }
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
            /*
             * Resource exhaustion. The listener is taken out of the epoll set
             * and the retry is scheduled on the timer wheel: leaving it
             * registered would make EPOLLIN refire at once and spin the loop
             * on a queue it cannot drain, and sleeping here would stall every
             * other connection for the duration of the sleep.
             */
            dx_log_write(w->log, DX_LOG_ERROR,
                         "accept4: %s (serving %zu connections, %zu accepted total)",
                         dx_strerror(errno), dx_pool_active(w->pool), w->accepted_total);
            accept_backoff(w);
            return;
        }
        dx_log_write(w->log, DX_LOG_ERROR, "accept4: %s", dx_strerror(errno));
        return;
    }
}

/*
 * Handle one event on the control channel.
 *
 * Multi-worker mode: ctl_fd is an eventfd carrying a DX_CMD_* bitmask written
 * by the main thread.
 *
 * Single-worker mode: ctl_fd is a signalfd; every readable signal is mapped
 * onto the same commands, so both modes run one dispatch path.
 */
static void handle_control(dx_worker_t *w)
{
    unsigned cmds = 0;

    if (w->ctl_is_signal) {
        for (;;) {
            struct signalfd_siginfo info;
            ssize_t n = read(w->ctl_fd, &info, sizeof(info));

            if (n != (ssize_t)sizeof(info)) {
                break; /* EAGAIN: the signal queue is drained */
            }
            switch (info.ssi_signo) {
            case SIGTERM:
            case SIGINT:
                cmds |= DX_CMD_SHUTDOWN;
                break;
            case SIGQUIT:
                cmds |= DX_CMD_FORCE;
                break;
            case SIGHUP:
                cmds |= DX_CMD_RELOAD;
                break;
            case SIGUSR1:
                cmds |= DX_CMD_REOPEN;
                break;
            case SIGUSR2:
                cmds |= DX_CMD_STATS;
                break;
            default:
                break; /* unexpected signal: ignore */
            }
        }
    } else {
        uint64_t value = 0;

        if (read(w->ctl_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
            cmds = (unsigned)value;
        }
    }

    if ((cmds & DX_CMD_FORCE) != 0u) {
        w->force_stop = true;
        return; /* the loop closes everything on the next turn */
    }
    if ((cmds & DX_CMD_SHUTDOWN) != 0u) {
        w->stop_requested = true;
    }
    if ((cmds & DX_CMD_RELOAD) != 0u) {
        dx_worker_apply_reload(w);
    }
    if ((cmds & DX_CMD_REOPEN) != 0u && w->reopen_cb != NULL) {
        w->reopen_cb(w->reopen_ctx);
    }
    if ((cmds & DX_CMD_STATS) != 0u) {
        dx_log_write(w->log, DX_LOG_INFO,
                     "worker %u stats (accepted %llu, rejected %llu, closed %llu)", w->worker_id,
                     (unsigned long long)w->accepted_total, (unsigned long long)w->rejected_total,
                     (unsigned long long)w->closed_total);
    }
}

/* Dispatch one event. Takes ownership of the connection lifetime. */
static void handle_event(dx_worker_t *w, const struct epoll_event *ev)
{
    uint64_t tag = ev->data.u64;
    unsigned kind = dx_tag_kind(tag);

    if (kind == DX_TAG_KIND_LISTENER) {
        accept_loop(w);
        return;
    }
    if (kind == DX_TAG_KIND_WAKE) {
        /*
         * A wakeup from the main thread (shutdown) or a signal on the
         * single-worker signalfd. Drain the channel and apply the commands;
         * the loop condition observes the flags on the next turn.
         */
        handle_control(w);
        return;
    }
    if (kind != DX_TAG_KIND_CONN) {
        return; /* unknown tag: nothing to dispatch */
    }

    {
        /*
         * Resolve through the pool so a recycled slot cannot be touched by a
         * stale event. conn_close may release the slot, hence the single use
         * of the pointer below.
         */
        dx_conn_t *conn = dx_pool_lookup(w->pool, dx_tag_index(tag), dx_tag_generation(tag));
        bool close_it = false;

        if (conn == NULL) {
            return;
        }

        if ((ev->events & (EPOLLERR | EPOLLHUP)) != 0) {
            close_it = true;
        } else if ((ev->events & (EPOLLIN | EPOLLRDHUP)) != 0) {
            /*
             * EPOLLRDHUP is handled through the readable path: unread bytes
             * may still be in flight, and reading until EOF is the only way
             * to learn whether the peer is done.
             */
            if (dx_conn_on_readable(w, conn) != 0) {
                close_it = true;
            } else if (conn_update_interest(w, conn) != 0) {
                close_it = true;
            }
        } else if ((ev->events & EPOLLOUT) != 0) {
            if (dx_conn_on_writable(w, conn) != 0) {
                close_it = true;
            } else if (conn_update_interest(w, conn) != 0) {
                close_it = true;
            }
        }
        /* EPOLLRDHUP alone still delivers a readable event with the EOF. */

        if (close_it) {
            conn_close(w, conn);
        }
    }
}

/*
 * Timeout policy. An idle connection is closed silently, a connection that
 * already received part of a request gets 408 before the close, and one that
 * is stuck sending or discarding a body is closed outright.
 */
static void conn_on_timeout(struct dx_worker *w, dx_conn_t *conn)
{
    const char *what;
    bool got_bytes;

    /*
     * Two sources, because the unread bytes can sit in two places: in the read
     * buffer (normal case) or only in the parser scan position, when the
     * connection went idle with its buffer released and a partial request
     * arrived afterwards. Either one means the client sent something and
     * deserves a 408 instead of a silent close.
     */
    got_bytes = (dx_buffer_readable(&conn->buf) > 0) || conn->parser.scan > 0;
    switch (conn->state) {
    case DX_CONN_IDLE:
        what = "keepalive timeout";
        break;
    case DX_CONN_WRITING:
        what = "send timeout";
        break;
    case DX_CONN_READING:
        what = (conn->body_left > 0) ? "body timeout" : "header timeout";
        break;
    default:
        return;
    }

    dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: %s, closing",
                 (unsigned long long)conn->id, what);

    if (got_bytes && conn->state == DX_CONN_READING && conn->body_left == 0) {
        /*
         * Part of a request arrived: answer 408 so the client learns why.
         * The partial request is dropped first, otherwise the client would
         * read its own bytes back before the error page.
         */
        char tmp[DX_RESPONSE_TMP];
        size_t len = dx_http_response_error(tmp, sizeof(tmp), 408, NULL);

        if (len > 0) {
            size_t keep = conn->buf.cap;
            bool flushed = false;

            dx_buffer_init(&conn->buf, conn->buf.data, keep);
            if (dx_buffer_writable(&conn->buf) >= len) {
                (void)dx_buffer_append(&conn->buf, tmp, len);
                dx_conn_record_status(conn, 408);
                conn->close_conn = true;
                /*
                 * Honour the result. The previous code ignored it and closed
                 * right away: on EAGAIN the response was still pending, so the
                 * client received a truncated error page (or none at all).
                 */
                if (dx_conn_on_writable(w, conn) != 0) {
                    flushed = false;
                } else {
                    flushed = !dx_conn_has_work(conn);
                }
            }
            if (flushed) {
                return; /* the 408 is on the wire; the close follows below */
            }
            if (dx_conn_has_work(conn)) {
                /*
                 * Still pending: let the EPOLLOUT path finish it. close_conn is
                 * already set, so the connection ends as soon as the response is
                 * out, and the send timeout bounds a peer that never reads.
                 */
                dx_conn_arm_timer(w, conn);
                return;
            }
        }
    }
    conn_close(w, conn);
}

static void on_timer_expired(void *ctx, dx_timer_node_t *node)
{
    struct dx_worker *w = (struct dx_worker *)ctx;

    if (node == &w->accept_timer) {
        /* The backoff elapsed: put the listener back and drain the queue. */
        if (w->draining) {
            return; /* shutting down: no new connections */
        }
        if (listener_arm(w) == 0) {
            accept_loop(w);
        }
        return;
    }
    {
        dx_conn_t *conn = (dx_conn_t *)node->owner;

        if (conn == NULL || conn->state == DX_CONN_FREE || conn->state == DX_CONN_CLOSING) {
            return; /* the slot was recycled: the timer is stale */
        }
        conn_on_timeout(w, conn);
    }
}

/*
 * Close everything still open; used on shutdown and after a fatal error.
 *
 * The loop must cover the pool's real slot space, not max_conns: slots are
 * handed out from the top of the free list, so the first connections of a
 * worker with --max-conns 2 live at indices 1023 and 1022. Scanning [0, 2)
 * found nothing, skipped the graceful close and lost the access log line of
 * every live connection.
 */
static void drain_connections(dx_worker_t *w)
{
    size_t total = dx_pool_total_slots(w->pool);
    size_t i;

    for (i = 0; i < total; i++) {
        dx_conn_t *conn = dx_pool_slot(w->pool, i);

        if (conn != NULL && conn->state != DX_CONN_FREE) {
            conn_close(w, conn);
        }
    }
}

int dx_worker_run(dx_worker_t *w)
{
    struct epoll_event *events;
    int rc = 0;

    if (w == NULL || w->epfd < 0) {
        errno = EINVAL;
        return -1;
    }

    events = (struct epoll_event *)calloc(DX_MAX_EVENTS, sizeof(*events));
    if (events == NULL) {
        dx_log_write(w->log, DX_LOG_ERROR, "cannot allocate the epoll event batch");
        return -1;
    }

    for (;;) {
        uint64_t now = (uint64_t)dx_now_ms();
        int timeout_ms = dx_timer_wheel_next_timeout_ms(w->timers, now);
        int n;
        int i;
        /*
         * Stop sources: the signal flag (single-worker), the control channel
         * (both modes) and SIGQUIT's immediate close. All are only touched by
         * this thread.
         */
        bool stopping = w->stop_requested || (w->stop_flag != NULL && *w->stop_flag != 0);

        if (w->force_stop) {
            dx_log_write(w->log, DX_LOG_WARN, "forced shutdown, closing %zu connection(s)",
                         dx_pool_active(w->pool));
            break;
        }

        if (!stopping && !w->draining) {
            /* Normal operation: wait for the nearest timer or an event. */
            n = epoll_wait(w->epfd, events, DX_MAX_EVENTS, timeout_ms);
        } else if (!w->draining) {
            /*
             * Graceful shutdown started: stop accepting, then wait for the
             * in-flight connections until the shutdown budget runs out.
             */
            w->draining = true;
            dx_timer_wheel_cancel(w->timers, &w->accept_timer);
            w->shutdown_deadline_ms = now + w->life.shutdown_timeout_ms;
            dx_log_write(w->log, DX_LOG_INFO, "draining %zu open connection(s) before shutdown",
                         dx_pool_active(w->pool));
            if (w->epfd >= 0 && w->listen_fd >= 0) {
                (void)epoll_ctl(w->epfd, EPOLL_CTL_DEL, w->listen_fd, NULL);
            }
            if (dx_pool_active(w->pool) == 0) {
                break;
            }
            n = epoll_wait(w->epfd, events, DX_MAX_EVENTS, DX_TIMER_TICK_MS);
        } else {
            if (dx_pool_active(w->pool) == 0) {
                break;
            }
            if (now >= w->shutdown_deadline_ms) {
                dx_log_write(w->log, DX_LOG_WARN,
                             "shutdown timeout expired, closing %zu connection(s) forcefully",
                             dx_pool_active(w->pool));
                drain_connections(w);
                break;
            }
            n = epoll_wait(w->epfd, events, DX_MAX_EVENTS, DX_TIMER_TICK_MS);
        }

        if (n < 0) {
            if (errno == EINTR) {
                /*
                 * A signal arrived. The handler only sets the stop flag,
                 * which the loop condition above observes on the next turn.
                 */
                continue;
            }
            dx_log_write(w->log, DX_LOG_ERROR, "epoll_wait: %s", dx_strerror(errno));
            rc = -1;
            break;
        }

        for (i = 0; i < n; i++) {
            handle_event(w, &events[i]);
        }

        /* Fire every timer that came due (timeouts, keep-alive, body, send). */
        (void)dx_timer_wheel_advance(w->timers, (uint64_t)dx_now_ms(), on_timer_expired, w);
    }

    drain_connections(w);
    free(events);
    return rc;
}
