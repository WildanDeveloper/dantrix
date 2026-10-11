#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

const char *dx_strerror(int err)
{
    const char *s = strerror(err);

    return (s != NULL) ? s : "unknown error";
}

const char *dx_sockerr(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return dx_strerror(errno);
    }
    return dx_strerror(err);
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
    if (v6) {
        /* Dual stack: accept IPv4 clients on an IPv6 wildcard listener. */
        int off = 0;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) {
            goto fail;
        }
    }

    if (bind(fd, (struct sockaddr *)&ss, ss_len) != 0) {
        goto fail;
    }
    if (listen(fd, backlog) != 0) {
        goto fail;
    }
    return fd;

fail:
    {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
}
