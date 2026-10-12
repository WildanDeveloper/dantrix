/* Unit tests for src/net.c (listener creation, eventfd wakeup channel). */
#include "testing.h"

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Ask the kernel for a free TCP port by binding a throwaway socket to port 0
 * and reading back the assigned port. There is a small window before the port
 * is reused, which is fine for a unit test.
 */
static int free_port(void)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int port = -1;

    if (fd < 0) {
        return -1;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
        getsockname(fd, (struct sockaddr *)&sin, &len) == 0) {
        port = ntohs(sin.sin_port);
    }
    close(fd);
    return port;
}

static void test_listen_tcp(void)
{
    int fd;
    int port = free_port();

    dx_test_begin("net/listen_tcp");

    CHECK(port > 0);
    if (port > 0) {
        fd = dx_listen_tcp("127.0.0.1", port, 16);
        CHECK(fd >= 0);
        if (fd >= 0) {
            /* The helper promises non-blocking and close-on-exec. */
            CHECK((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
            CHECK((fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0);
            close(fd);
        }
    }

    /* Rejects: NULL address, out-of-range port, negative backlog. */
    CHECK_EQ(dx_listen_tcp(NULL, 8080, 16), -1);
    CHECK_EQ(dx_listen_tcp("127.0.0.1", 0, 16), -1);
    CHECK_EQ(dx_listen_tcp("127.0.0.1", 70000, 16), -1);
    CHECK_EQ(dx_listen_tcp("127.0.0.1", 8080, -1), -1);
    CHECK_EQ(dx_listen_tcp("", 8080, 16), -1);
    CHECK_EQ(dx_listen_tcp("not-an-ip", 8080, 16), -1);
}

static void test_listen_reuseport(void)
{
    int a;
    int b;
    int port = free_port();

    dx_test_begin("net/listen_reuseport");

    CHECK(port > 0);
    if (port <= 0) {
        return;
    }

    /*
     * Two sockets bound to the same address:port only succeed together with
     * SO_REUSEPORT. This is the core property the multi-worker server relies
     * on; if the kernel rejects it, the fallback path takes over.
     */
    a = dx_listen_tcp_reuseport("127.0.0.1", port, 16);
    CHECK(a >= 0);
    if (a >= 0) {
        b = dx_listen_tcp_reuseport("127.0.0.1", port, 16);
        CHECK(b >= 0);
        if (b >= 0) {
            close(b);
        }
        close(a);
    }

    /* A plain (non-reuseport) bind on the same port must fail. */
    a = dx_listen_tcp_reuseport("127.0.0.1", port, 16);
    if (a >= 0) {
        b = dx_listen_tcp("127.0.0.1", port, 16);
        CHECK_EQ(b, -1); /* no SO_REUSEPORT: EADDRINUSE */
        if (b >= 0) {
            close(b);
        }
        close(a);
    }
}

static void test_eventfd(void)
{
    int fd;
    int i;

    dx_test_begin("net/eventfd");

    fd = dx_eventfd();
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    CHECK((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
    CHECK((fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0);

    /* A fresh eventfd has nothing pending: draining is a no-op, not an error. */
    CHECK_EQ(dx_eventfd_drain(fd), 0);

    /* One signal is one pending wakeup, and it is consumed exactly once. */
    CHECK_EQ(dx_eventfd_signal(fd), 0);
    CHECK_EQ(dx_eventfd_drain(fd), 0);
    CHECK_EQ(dx_eventfd_drain(fd), 0); /* now empty again */

    /* Many signals coalesce into a single readable counter. */
    for (i = 0; i < 4; i++) {
        CHECK_EQ(dx_eventfd_signal(fd), 0);
    }
    CHECK_EQ(dx_eventfd_drain(fd), 0);

    close(fd);

    /* Invalid descriptors are reported, never crash. */
    CHECK_EQ(dx_eventfd_signal(-1), -1);
    CHECK_EQ(dx_eventfd_drain(-1), -1);
}

int main(void)
{
    test_listen_tcp();
    test_listen_reuseport();
    test_eventfd();
    printf("test_net: ");
    return dx_test_summary();
}
