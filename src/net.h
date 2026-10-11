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

/* Put an already open fd into non-blocking mode. Returns 0 or -1. */
int dx_set_nonblock(int fd);

/* Enable TCP_NODELAY (no Nagle delay). Returns 0 or -1. */
int dx_set_tcp_nodelay(int fd);

/* Human readable name for an errno value; never returns NULL. */
const char *dx_strerror(int err);

#endif /* DX_NET_H */
