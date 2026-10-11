/*
 * examples/03_echo_epoll.c - The scalable core: one thread, epoll, non-blocking.
 *
 * Learning goal: the readiness model that Dantrix itself is built on. A single
 * thread watches every descriptor; nothing blocks, so one thread serves tens of
 * thousands of connections with a handful of syscalls per event.
 *
 * Key ideas demonstrated here:
 *  - every socket is non-blocking; EAGAIN means "come back later", not an error
 *  - level-triggered epoll: EPOLLIN stays reported while data is unread, so a
 *    partial read is safe
 *  - EPOLLOUT is registered only while output is pending, otherwise the loop
 *    spins forever on an always-writable socket
 *  - read/write loops must handle EINTR and partial transfers
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -o build/03_echo_epoll examples/03_echo_epoll.c
 * Try:   for i in $(seq 1 5000); do nc 127.0.0.1 9002 </dev/null & done
 *        (compare memory with example 02 doing the same)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9002
#define BACKLOG 4096
#define MAX_EVENTS 256

/* Per-connection state kept by index, never by raw pointer, in real servers. */
struct conn {
    int fd;
    size_t off;              /* first byte of pending output */
    size_t len;              /* end of valid data in buf */
    unsigned char buf[4096]; /* read buffer and pending echo window */
};

static struct conn conns[FD_SETSIZE];
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void conn_close(int epfd, struct conn *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
}

/* Flush pending output; returns -1 to close, 0 when more can be read. */
static int conn_flush(int epfd, struct epoll_event *ev, struct conn *c)
{
    while (c->off < c->len) {
        ssize_t sent = send(c->fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL);

        if (sent > 0) {
            c->off += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Still pending: keep EPOLLOUT registered to resume later. */
            ev->events = EPOLLIN | EPOLLOUT;
            epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, ev);
            return 0;
        }
        return -1;
    }
    c->off = c->len = 0;
    /* Drained: drop EPOLLOUT or the loop would spin. */
    ev->events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, ev);
    return 0;
}

int main(void)
{
    struct sockaddr_in addr;
    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];
    int epfd;
    int listen_fd;
    int i;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    {
        int on = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, BACKLOG) != 0) {
        perror("listen");
        return 1;
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    /* Static storage starts at 0; free slots are marked with fd = -1. */
    for (i = 0; i < FD_SETSIZE; i++) {
        conns[i].fd = -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = -1; /* negative fd marks the listener slot */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
        perror("epoll_ctl");
        return 1;
    }
    printf("listening on port %d (epoll, single thread)\n", PORT);

    while (!g_stop) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        int e;

        if (n < 0) {
            if (errno == EINTR) {
                continue; /* the stop flag is checked by the loop condition */
            }
            perror("epoll_wait");
            return 1;
        }

        for (e = 0; e < n; e++) {
            if (events[e].data.fd == -1) {
                /* Listener: accept until the queue is empty. */
                for (;;) {
                    int fd = accept4(listen_fd, NULL, NULL,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
                    struct conn *c;
                    int j;

                    if (fd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break; /* EAGAIN or transient error: done for now */
                    }
                    for (j = 0; j < FD_SETSIZE && conns[j].fd != -1; j++) {
                    }
                    if (j == FD_SETSIZE) {
                        close(fd); /* out of slots; a real server grows here */
                        continue;
                    }
                    c = &conns[j];
                    c->fd = fd;
                    c->off = c->len = 0;
                    memset(&ev, 0, sizeof(ev));
                    ev.events = EPOLLIN;
                    ev.data.fd = j;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
                        close(fd);
                        c->fd = -1;
                    }
                }
                continue;
            }

            {
                struct conn *c = &conns[events[e].data.fd];

                if ((events[e].events & (EPOLLERR | EPOLLHUP)) != 0) {
                    conn_close(epfd, c);
                    continue;
                }
                if ((events[e].events & (EPOLLIN | EPOLLRDHUP)) != 0) {
                    /* Read until EAGAIN or EOF: level-triggered demands it. */
                    for (;;) {
                        ssize_t nread = read(c->fd, c->buf + c->len,
                                             sizeof(c->buf) - c->len);

                        if (nread > 0) {
                            c->len += (size_t)nread;
                            if (c->len == sizeof(c->buf)) {
                                break; /* full: flush before reading more */
                            }
                            continue;
                        }
                        if (nread == 0) {
                            /* EOF: echo what arrived, then close. */
                            if (conn_flush(epfd, &events[e], c) != 0) {
                                conn_close(epfd, c);
                            } else if (c->off == c->len) {
                                conn_close(epfd, c);
                            }
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        conn_close(epfd, c);
                        break;
                    }
                    if (c->fd != -1 && c->len > c->off) {
                        if (conn_flush(epfd, &events[e], c) != 0) {
                            conn_close(epfd, c);
                        }
                    }
                } else if ((events[e].events & EPOLLOUT) != 0) {
                    if (conn_flush(epfd, &events[e], c) != 0) {
                        conn_close(epfd, c);
                    }
                }
            }
        }
    }

    printf("shutting down\n");
    close(epfd);
    close(listen_fd);
    return 0;
}
