/*
 * net.h - Socket helpers. Every descriptor returned here is non-blocking and
 * close-on-exec, so callers never have to remember to set those flags.
 */
#ifndef DX_NET_H
#define DX_NET_H

#include <stdbool.h>

/*
 * Create a listening TCP socket bound to addr:port.
 *
 * The socket is created with SOCK_NONBLOCK | SOCK_CLOEXEC, SO_REUSEADDR is set,
 * and IPv6 sockets are made dual stack. Returns the fd or -1 with errno set.
 * The caller owns the fd.
 */
int dx_listen_tcp(const char *addr, int port, int backlog);

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
