/*
 * net.h - Socket helpers. Every descriptor returned here is non-blocking and
 * close-on-exec, so callers never have to remember to set those flags.
 */
#ifndef DX_NET_H
#define DX_NET_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Listener socket tuning. Every field is optional: 0 means "leave the kernel
 * default alone". These were accepted by the configuration before but were
 * never applied, so an operator's tuning silently did nothing.
 */
typedef struct {
    unsigned defer_accept_ms; /* TCP_DEFER_ACCEPT: queue until data arrives */
    unsigned fastopen_qlen;   /* TCP_FASTOPEN accept queue length */
    size_t rcvbuf;            /* SO_RCVBUF for the listener (inherited) */
    size_t sndbuf;            /* SO_SNDBUF for the listener (inherited) */
} dx_sockopts_t;

/*
 * Create a listening TCP socket bound to addr:port.
 *
 * The socket is created with SOCK_NONBLOCK | SOCK_CLOEXEC, SO_REUSEADDR is set,
 * and IPv6 sockets are made dual stack. Returns the fd or -1 with errno set.
 * The caller owns the fd.
 */
int dx_listen_tcp(const char *addr, int port, int backlog);

/*
 * Extended form: additionally sets SO_REUSEPORT when reuseport is true and
 * applies *opts (when non-NULL) before bind(). A tuning option the kernel
 * refuses is left at its default and reported in *out_refused (when non-NULL)
 * by setting only that field; it is never a fatal error, because a host whose
 * kernel lacks one of the tunables is still perfectly able to serve. The
 * caller logs what was refused.
 */
int dx_listen_tcp_ex(const char *addr, int port, int backlog, bool reuseport,
                     const dx_sockopts_t *opts, dx_sockopts_t *out_refused);

/*
 * Like dx_listen_tcp(), but additionally sets SO_REUSEPORT before bind() so
 * that several listeners (one per worker) can share the same address and port.
 * The kernel then spreads incoming connections across them by 4-tuple hash,
 * which removes the single acceptor as a contention point. Same return
 * contract as dx_listen_tcp().
 */
int dx_listen_tcp_reuseport(const char *addr, int port, int backlog);

/*
 * Create an eventfd in non-blocking close-on-exec mode. Returns the fd or -1
 * with errno set. The multi-worker server uses one per worker to wake it out
 * of epoll_wait() from the main thread.
 */
int dx_eventfd(void);

/*
 * Create a signalfd that reports the signals in mask. The signals must also be
 * blocked (pthread_sigmask) or the default disposition fires before the fd is
 * ever read. Returns the fd or -1 with errno set.
 *
 * blocking selects the wait mode: the single-worker path registers the fd in
 * epoll (non-blocking, read in a loop until EAGAIN), while the orchestrator
 * waits on the fd directly and needs a blocking read.
 */
int dx_signalfd_create(const sigset_t *mask, bool blocking);

/*
 * Add 1 to an eventfd's counter. A saturated counter (EAGAIN) is treated as
 * success: the worker is already flagged to wake up. Returns 0 or -1.
 */
int dx_eventfd_signal(int fd);

/* Consume the eventfd's pending counter. 0 also when nothing was pending. */
int dx_eventfd_drain(int fd);

/* Put an already open fd into non-blocking mode. Returns 0 or -1. */
int dx_set_nonblock(int fd);

/* Enable TCP_NODELAY (no Nagle delay). Returns 0 or -1. */
int dx_set_tcp_nodelay(int fd);

/* Human readable name for an errno value; never returns NULL. */
const char *dx_strerror(int err);

#endif /* DX_NET_H */
