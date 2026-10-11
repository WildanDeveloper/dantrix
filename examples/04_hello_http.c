/*
 * examples/04_hello_http.c - The step from bytes to HTTP.
 *
 * Learning goal: recognise the end of an HTTP request header block (the empty
 * line "\r\n\r\n") and answer with a minimal, well formed response. Everything
 * after this example (incremental parsing, methods, status codes) grows in
 * src/http_parser.c from v0.2 onwards.
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -o build/04_hello_http examples/04_hello_http.c
 * Try:   curl -i http://127.0.0.1:9003/
 *        curl -i http://127.0.0.1:9003/anything/else
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

#define PORT 9003
#define BACKLOG 64

/* A complete, fixed response: no client input is reflected back (XSS rule). */
static const char RESPONSE[] = "HTTP/1.1 200 OK\r\n"
                               "Server: dx-example\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: 15\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "hello, HTTP!\r\n";

/* Send one blocking write, retrying on EINTR and short writes. */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t sent = send(fd, buf + off, len - off, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            /* No progress and no error: retrying would spin forever. */
            return -1;
        }
        off += (size_t)sent;
    }
    return 0;
}

/* Serve one blocking connection: read until the header block is complete. */
static void handle(int fd)
{
    char buf[8192];
    size_t len = 0;

    while (len < sizeof(buf)) {
        ssize_t n = read(fd, buf + len, sizeof(buf) - len);
        char *end;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return; /* client gave up */
        }
        len += (size_t)n;

        /* The request ends at the first empty line. */
        end = strstr(buf, "\r\n\r\n");
        if (end != NULL) {
            (void)write_all(fd, RESPONSE, sizeof(RESPONSE) - 1);
            return;
        }
    }
    /* Header larger than the buffer: close without a response. */
}

int main(void)
{
    struct sockaddr_in addr;
    int listen_fd;

    signal(SIGPIPE, SIG_IGN);

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
    printf("listening on port %d (blocking, one request at a time)\n", PORT);

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);

        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return 1;
        }
        handle(conn_fd);
        close(conn_fd);
    }
}
