#include "worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dantrix.h"
#include "net.h"

#include <time.h>

dx_log_t *dx_worker_log(dx_worker_t *w)
{
    return (w == NULL) ? NULL : w->log;
}

int dx_worker_init(dx_worker_t *w, int listen_fd, size_t max_conns, dx_log_t *log,
                   const volatile sig_atomic_t *stop_flag, const dx_http_limits_t *limits,
                   dx_log_t *access_log, dx_log_format_t log_format, dx_fs_t *fs, dx_mime_t *mime,
                   const dx_static_opts_t *static_opts)
{
    struct epoll_event ev;

    if (w == NULL || listen_fd < 0 || max_conns == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(w, 0, sizeof(*w));
    w->epfd = -1;
    w->listen_fd = listen_fd;
    w->log = log;
    w->access_log = access_log;
    w->log_format = log_format;
    w->fs = fs;
    w->mime = mime;
    if (limits != NULL) {
        w->limits = *limits;
    } else {
        dx_http_limits_default(&w->limits);
    }
    if (static_opts != NULL) {
        w->static_opts = *static_opts;
    }
    if (w->static_opts.cache_control == NULL) {
        w->static_opts.cache_control = "";
    }
    if (w->static_opts.sendfile_chunk == 0) {
        w->static_opts.sendfile_chunk = 1024u * 1024u;
    }
    w->max_conns = max_conns;
    w->stop_flag = stop_flag;

    w->pool = dx_pool_create(DX_CONN_CHUNK_SLOTS, max_conns);
    if (w->pool == NULL) {
        errno = ENOMEM;
        return -1;
    }
    w->bufs = dx_bufpool_create(DX_CONN_BUF_SIZE, DX_BUF_CHUNK_SLOTS);
    if (w->bufs == NULL) {
        errno = ENOMEM;
        return -1;
    }

    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (w->epfd < 0) {
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = dx_tag_listener();
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
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
    if (w->listen_fd >= 0) {
        (void)close(w->listen_fd);
        w->listen_fd = -1;
    }
    dx_pool_destroy(w->pool);
    w->pool = NULL;
    dx_bufpool_destroy(w->bufs);
    w->bufs = NULL;
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
        dx_log_write(w->log, DX_LOG_WARN,
                     "connection rejected: read buffer pool exhausted "
                     "(%zu buffers in use)",
                     dx_bufpool_outstanding(w->bufs));
        return -1;
    }

    conn = dx_pool_acquire(w->pool);
    if (conn == NULL) {
        (void)dx_bufpool_release(w->bufs, mem);
        dx_log_write(w->log, DX_LOG_WARN, "connection rejected: limit of %zu connections reached",
                     w->max_conns);
        return -1;
    }

    dx_conn_init(conn, fd, &w->limits);
    conn->buf_mem = mem;
    dx_buffer_init(&conn->buf, mem, DX_CONN_BUF_SIZE);
    format_peer(peer, conn->peer, sizeof(conn->peer));

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.u64 = dx_tag_conn(conn->index, conn->generation);
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        int saved = errno;

        (void)close(fd);
        dx_conn_reset(conn);
        (void)dx_pool_release(w->pool, conn);
        (void)dx_bufpool_release(w->bufs, mem);
        dx_log_write(w->log, DX_LOG_ERROR, "epoll_ctl(ADD): %s", dx_strerror(saved));
        errno = saved;
        return -1;
    }

    /* TCP_NODELAY only changes latency; a failure must not drop the client. */
    (void)dx_set_tcp_nodelay(fd);
    w->accepted_total++;
    return 0;
}

/* Emit the access log line for the request that was served, if any. */
static void access_emit(dx_worker_t *w, const dx_conn_t *conn)
{
    const dx_access_t *acc = &conn->acc;

    if (w->access_log == NULL || acc->status == 0) {
        return; /* disabled, or the request never reached a response */
    }
    dx_log_access(w->access_log, w->log_format, conn->peer, acc->method, strlen(acc->method),
                  acc->target, strlen(acc->target), acc->http10 ? "HTTP/1.0" : "HTTP/1.1",
                  acc->status, acc->body_bytes, acc->referer, acc->user_agent);
}

/* Tear a connection down and give its slot and buffer back to the pools. */
static void conn_close(dx_worker_t *w, dx_conn_t *conn)
{
    int fd;

    if (conn == NULL || conn->state == DX_CONN_FREE) {
        return;
    }
    conn->state = DX_CONN_CLOSING;

    fd = conn->fd;
    conn->fd = -1;

    if (w->epfd >= 0 && fd >= 0) {
        (void)epoll_ctl(w->epfd, EPOLL_CTL_DEL, fd, NULL);
    }
    if (fd >= 0) {
        (void)close(fd);
    }

    /* One line per completed request, emitted while the record is intact. */
    access_emit(w, conn);

    /* A streamed file is owned by the connection until it is closed. */
    dx_conn_release_file(conn);

    /* Returns the pooled slot or frees the grown heap allocation. */
    dx_conn_release_buffer(conn, w->bufs);
    dx_conn_reset(conn);
    (void)dx_pool_release(w->pool, conn);
    w->closed_total++;
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
    if (conn->eof || conn->close_after_response) {
        /*
         * Nothing more will be read: after an EOF read() would return 0
         * forever, and with the response staged the request is over. Only
         * EPOLLOUT stays registered while the reply (headers or file body)
         * drains.
         */
        if (!dx_conn_has_work(conn)) {
            return -1; /* nothing left to send: the caller closes */
        }
        ev.events = EPOLLOUT;
    } else {
        ev.events = EPOLLIN | EPOLLRDHUP;
        if (conn->state == DX_CONN_WRITING) {
            ev.events |= EPOLLOUT;
        }
    }
    ev.data.u64 = dx_tag_conn(conn->index, conn->generation);
    return epoll_ctl(w->epfd, EPOLL_CTL_MOD, conn->fd, &ev);
}

/* Accept until the queue is empty. Never blocks: the socket is non-blocking. */
static void accept_loop(dx_worker_t *w)
{
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept4(w->listen_fd, (struct sockaddr *)&peer, &peer_len,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd >= 0) {
            if (conn_open(w, fd, &peer) != 0) {
                /*
                 * conn_open consumed the descriptor on every failure path, so
                 * there is nothing left to close here.
                 */
                w->rejected_total++;
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
             * Resource exhaustion. The listener stays registered, but with a
             * non-empty accept queue EPOLLIN would refire immediately and turn
             * this into a 100% CPU spin until descriptors free up. Backing off
             * briefly keeps the loop alive without burning a core. The timer
             * wheel (v0.4) replaces this sleep with a proper delayed retry.
             */
            dx_log_write(w->log, DX_LOG_ERROR,
                         "accept4: %s (serving %zu connections, %zu accepted total)",
                         dx_strerror(errno), dx_pool_active(w->pool), w->accepted_total);
            {
                struct timespec backoff = {0, 100 * 1000 * 1000}; /* 100 ms */

                (void)nanosleep(&backoff, NULL);
            }
            return;
        }
        dx_log_write(w->log, DX_LOG_ERROR, "accept4: %s", dx_strerror(errno));
        return;
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

/* Close everything still open; used on shutdown and after a fatal error. */
static void drain_connections(dx_worker_t *w)
{
    size_t i;

    for (i = 0; i < w->max_conns; i++) {
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

    while (w->stop_flag == NULL || *w->stop_flag == 0) {
        int n = epoll_wait(w->epfd, events, DX_MAX_EVENTS, -1);
        int i;

        if (n < 0) {
            if (errno == EINTR) {
                /*
                 * A signal arrived. The handler only sets the stop flag, which
                 * the loop condition above observes on the next turn.
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
    }

    if (w->stop_flag != NULL && *w->stop_flag != 0) {
        dx_log_write(w->log, DX_LOG_INFO, "draining %zu open connection(s) before shutdown",
                     dx_pool_active(w->pool));
    }
    drain_connections(w);
    free(events);
    return rc;
}
