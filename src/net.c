#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int dx_listen_tcp_common(const char *addr, int port, int backlog, bool reuseport,
                                const dx_sockopts_t *opts, dx_sockopts_t *out_refused);

const char *dx_strerror(int err)
{
    const char *s = strerror(err);

    return (s != NULL) ? s : "unknown error";
}

int dx_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

int dx_set_tcp_nodelay(int fd)
{
    int on = 1;

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        return -1;
    }
    return 0;
}

int dx_listen_tcp(const char *addr, int port, int backlog)
{
    return dx_listen_tcp_common(addr, port, backlog, false, NULL, NULL);
}

int dx_listen_tcp_reuseport(const char *addr, int port, int backlog)
{
    return dx_listen_tcp_common(addr, port, backlog, true, NULL, NULL);
}

int dx_listen_tcp_ex(const char *addr, int port, int backlog, bool reuseport,
                     const dx_sockopts_t *opts, dx_sockopts_t *out_refused)
{
    return dx_listen_tcp_common(addr, port, backlog, reuseport, opts, out_refused);
}

static int dx_listen_tcp_common(const char *addr, int port, int backlog, bool reuseport,
                                const dx_sockopts_t *opts, dx_sockopts_t *out_refused)
{
    struct sockaddr_storage ss;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
    socklen_t ss_len = 0;
    int fd = -1;
    int on = 1;
    bool v6 = false;

    if (addr == NULL || port < 1 || port > 65535 || backlog < 0) {
        errno = EINVAL;
        return -1;
    }
    if (addr[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    memset(&ss, 0, sizeof(ss));
    if (strcmp(addr, "0.0.0.0") == 0) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons((unsigned short)port);
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        ss_len = sizeof(*sin);
    } else if (inet_pton(AF_INET, addr, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons((unsigned short)port);
        ss_len = sizeof(*sin);
    } else if (inet_pton(AF_INET6, addr, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons((unsigned short)port);
        ss_len = sizeof(*sin6);
        v6 = true;
    } else {
        /*
         * Not an IP literal. Host name resolution is out of scope for v0.1:
         * it would add a blocking resolver call to the startup path.
         */
        errno = EINVAL;
        return -1;
    }

    fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        goto fail;
    }
    if (reuseport) {
        /*
         * SO_REUSEPORT must be set on every listener before bind(); the kernel
         * then load-balances new connections across the sockets sharing the
         * 4-tuple. Without it the second bind() would fail with EADDRINUSE.
         */
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) {
            goto fail;
        }
    }
    if (v6) {
        /* Dual stack: accept IPv4 clients on an IPv6 wildcard listener. */
        int off = 0;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) {
            goto fail;
        }
    }

    /*
     * Tuning. Every field is optional and a refusal is reported rather than
     * fatal: a kernel without the option is still able to serve. All four
     * must be set before listen() to take effect.
     */
    if (opts != NULL) {
        if (out_refused != NULL) {
            memset(out_refused, 0, sizeof(*out_refused));
        }
        if (opts->rcvbuf > 0) {
            int value = (opts->rcvbuf > (size_t)INT_MAX) ? INT_MAX : (int)opts->rcvbuf;

            if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) != 0 &&
                out_refused != NULL) {
                out_refused->rcvbuf = opts->rcvbuf;
            }
        }
        if (opts->sndbuf > 0) {
            int value = (opts->sndbuf > (size_t)INT_MAX) ? INT_MAX : (int)opts->sndbuf;

            if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) != 0 &&
                out_refused != NULL) {
                out_refused->sndbuf = opts->sndbuf;
            }
        }
        if (opts->defer_accept_ms > 0) {
            /* The kernel option is expressed in seconds, rounded up. */
            int seconds = (int)((opts->defer_accept_ms + 999u) / 1000u);

            if (seconds < 1) {
                seconds = 1;
            }
            if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &seconds, sizeof(seconds)) != 0 &&
                out_refused != NULL) {
                out_refused->defer_accept_ms = opts->defer_accept_ms;
            }
        }
        if (opts->fastopen_qlen > 0) {
            int qlen =
                (opts->fastopen_qlen > (unsigned)INT_MAX) ? INT_MAX : (int)opts->fastopen_qlen;

            /*
             * Requires net.ipv4.tcp_fastopen to have the server bit (0x2) set;
             * on hosts where it does not, this fails with EOPNOTSUPP and the
             * server simply runs without it.
             */
            if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) != 0 &&
                out_refused != NULL) {
                out_refused->fastopen_qlen = opts->fastopen_qlen;
            }
        }
    }

    if (bind(fd, (struct sockaddr *)&ss, ss_len) != 0) {
        goto fail;
    }
    if (listen(fd, backlog) != 0) {
        goto fail;
    }
    return fd;

fail: {
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
}
}

int dx_eventfd(void)
{
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

int dx_signalfd_create(const sigset_t *mask, bool blocking)
{
    int flags = (blocking) ? SFD_CLOEXEC : (SFD_NONBLOCK | SFD_CLOEXEC);

    if (mask == NULL) {
        errno = EINVAL;
        return -1;
    }
    return signalfd(-1, mask, flags);
}

int dx_eventfd_signal(int fd)
{
    uint64_t one = 1;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (write(fd, &one, sizeof(one)) == (ssize_t)sizeof(one)) {
        return 0;
    }
    if (errno == EAGAIN) {
        /* Counter saturated: the wakeup is already pending, so this is fine. */
        return 0;
    }
    return -1;
}

int dx_eventfd_drain(int fd)
{
    uint64_t value = 0;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0; /* nothing was pending: not an error */
    }
    return -1;
}
