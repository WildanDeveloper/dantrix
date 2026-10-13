/*
 * idle_conns - hold N idle TCP connections open for a fixed time.
 *
 * Purpose: the C10K scenario (S5). It opens connections to a Dantrix
 * instance, optionally completes one request on each so the server has fully
 * established state, then idles for the requested duration and reports what it
 * observed. The server side is measured separately (RSS, fd count, stability).
 *
 * Usage:
 *   idle_conns [-h HOST] [-p PORT] [-n COUNT] [-t SECONDS] [-s SOURCE_IP,...]
 *              [-r] [-q]
 *
 *   -h HOST          server address                [127.0.0.1]
 *   -p PORT          server port                   [8080]
 *   -n COUNT         number of connections         [10000]
 *   -t SECONDS       how long to hold them         [60]
 *   -s IP,IP,...     source addresses to bind from (gets past the ~28k port
 *                    limit of a single source IP)
 *   -r               send one request per connection before idling
 *   -q               quiet: only the final summary
 *
 * Build: gcc -O2 -o build/idle_conns bench/tools/idle_conns.c
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOURCES 64

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-h HOST] [-p PORT] [-n COUNT] [-t SECONDS] [-s IP,...] [-r] [-q]\n",
            prog);
}

/* One source address pool entry, cycled through so the 4-tuple stays unique. */
typedef struct {
    struct in_addr addr;
} source_t;

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 8080;
    long count = 10000;
    long hold_s = 60;
    int send_request = 0;
    int quiet = 0;
    source_t sources[MAX_SOURCES];
    size_t n_sources = 1;
    int *fds = NULL;
    long opened = 0;
    long failed = 0;
    int opt;
    long i;
    struct sockaddr_in dst;

    sources[0].addr.s_addr = htonl(INADDR_LOOPBACK);

    while ((opt = getopt(argc, argv, "h:p:n:t:s:rq")) != -1) {
        switch (opt) {
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'n':
            count = atol(optarg);
            break;
        case 't':
            hold_s = atol(optarg);
            break;
        case 's': {
            char *save = NULL;
            char *tok = strtok_r(optarg, ",", &save);

            n_sources = 0;
            while (tok != NULL && n_sources < MAX_SOURCES) {
                if (inet_pton(AF_INET, tok, &sources[n_sources].addr) == 1) {
                    n_sources++;
                } else {
                    fprintf(stderr, "idle_conns: bad source address '%s'\n", tok);
                    return 2;
                }
                tok = strtok_r(NULL, ",", &save);
            }
            if (n_sources == 0) {
                fprintf(stderr, "idle_conns: no valid source addresses\n");
                return 2;
            }
            break;
        }
        case 'r':
            send_request = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (count <= 0 || hold_s < 0 || port < 1 || port > 65535) {
        usage(argv[0]);
        return 2;
    }

    /* A short, well-known request; Connection: keep-alive keeps the socket. */
    static const char request[] = "GET / HTTP/1.1\r\nHost: idle\r\nConnection: keep-alive\r\n\r\n";

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        fprintf(stderr, "idle_conns: bad host '%s'\n", host);
        return 2;
    }

    fds = (int *)malloc((size_t)count * sizeof(int));
    if (fds == NULL) {
        fprintf(stderr, "idle_conns: out of memory for %ld descriptors\n", count);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (i = 0; i < count && !g_stop; i++) {
        struct sockaddr_in src;
        int fd;
        int on = 1;

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            failed++;
            continue;
        }
        /* Rotate the source address so a single host can exceed one IP's
         * ephemeral port range (about 28k on default Linux settings). */
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        src.sin_addr = sources[(size_t)i % n_sources].addr;
        src.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
            close(fd);
            failed++;
            continue;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
            close(fd);
            failed++;
            continue;
        }
        if (send_request) {
            ssize_t n = send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL);

            if (n <= 0) {
                close(fd);
                failed++;
                continue;
            }
            /* Bound the drain so a stalled server cannot hang the tool. */
            {
                struct timeval tv;

                tv.tv_sec = 2;
                tv.tv_usec = 0;
                (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }
            /* Drain the whole response, otherwise the unread body bytes keep
             * POLLIN set for the rest of the hold and the summary would report
             * every connection as dropped. */
            {
                char buf[4096];
                size_t have = 0;
                size_t need = 0;
                int seen_header = 0;

                for (;;) {
                    ssize_t got = recv(fd, buf + have, sizeof(buf) - have, 0);

                    if (got <= 0) {
                        break;
                    }
                    have += (size_t)got;
                    if (!seen_header) {
                        const char *sep = memmem(buf, have, "\r\n\r\n", 4u);

                        if (sep == NULL) {
                            if (have == sizeof(buf)) {
                                break; /* header larger than the buffer: give up */
                            }
                            continue;
                        }
                        seen_header = 1;
                        need = (size_t)(sep - buf) + 4u;
                        {
                            const char *cl = strcasestr(buf, "content-length:");

                            if (cl != NULL && cl < sep) {
                                need += (size_t)strtoul(cl + 15, NULL, 10);
                            }
                        }
                    }
                    if (have >= need) {
                        break;
                    }
                }
            }
        }
        fds[opened++] = fd;
        if (!quiet && opened % 1000 == 0) {
            fprintf(stderr, "idle_conns: %ld/%ld open\n", opened, count);
        }
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double open_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("opened %ld/%ld connections in %.2fs", opened, count, open_s);
    if (failed > 0) {
        printf(" (%ld failed)", failed);
    }
    printf("\n");
    fflush(stdout);

    /* Hold them idle, polling for early death so a server that drops the
     * connections is reported instead of hidden. */
    long dropped = 0;
    if (opened > 0 && hold_s > 0) {
        struct timespec hold_start;
        clock_gettime(CLOCK_MONOTONIC, &hold_start);
        while (!g_stop) {
            struct timespec now;
            long elapsed;

            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed = (long)(now.tv_sec - hold_start.tv_sec);
            if (elapsed >= hold_s) {
                break;
            }
            /* A zero-length read: POLLIN means the peer closed (EOF). */
            {
                struct pollfd *pfds = (struct pollfd *)malloc((size_t)opened * sizeof(*pfds));
                long k;

                if (pfds == NULL) {
                    break;
                }
                for (k = 0; k < opened; k++) {
                    pfds[k].fd = fds[k];
                    pfds[k].events = POLLIN;
                    pfds[k].revents = 0;
                }
                (void)poll(pfds, (nfds_t)opened, 1000);
                for (k = 0; k < opened; k++) {
                    if (fds[k] < 0) {
                        continue; /* already counted */
                    }
                    /*
                     * Only HUP/ERR mean the server dropped the connection. A
                     * plain POLLIN would just be a late response byte and must
                     * not be reported as a drop.
                     */
                    if ((pfds[k].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                        dropped++;
                        fds[k] = -1;
                    }
                }
                free(pfds);
            }
        }
    }

    for (i = 0; i < opened; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    free(fds);

    printf("summary: opened %ld, failed %ld, dropped while idle %ld, held %lds\n", opened, failed,
           dropped, hold_s);
    return (opened == count) ? 0 : 1;
}
