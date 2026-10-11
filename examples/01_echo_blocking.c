/*
 * examples/01_echo_blocking.c - The starting point: a blocking echo server.
 *
 * Learning goal: the raw socket API. This server handles exactly one client at
 * a time; try opening two terminals with `nc 127.0.0.1 9000` and observe that
 * the second one waits until the first disconnects. That limitation is the
 * whole motivation for the event driven designs in the later examples.
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -o build/01_echo_blocking examples/01_echo_blocking.c
 * Try:   nc 127.0.0.1 9000
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9000
#define BACKLOG 16

int main(void)
{
    struct sockaddr_in addr;
    int listen_fd;
    int conn_fd;

    signal(SIGPIPE, SIG_IGN); /* a dead peer must not kill the process */

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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
    printf("listening on port %d (one client at a time)\n", PORT);

    for (;;) {
        char buf[4096];
        ssize_t n;

        /* Blocks until a client connects. Nothing else can happen meanwhile. */
        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return 1;
        }
        printf("client connected\n");

        /* Blocks on every read and every write. */
        while ((n = read(conn_fd, buf, sizeof(buf))) > 0) {
            ssize_t off = 0;

            while (off < n) {
                ssize_t sent = write(conn_fd, buf + off, (size_t)(n - off));

                if (sent < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("write");
                    break;
                }
                if (sent == 0) {
                    /* No progress and no error: retrying would spin forever. */
                    break;
                }
                off += sent;
            }
        }
        if (n < 0 && errno != ECONNRESET) {
            perror("read");
        }
        close(conn_fd);
        printf("client disconnected\n");
    }
}
