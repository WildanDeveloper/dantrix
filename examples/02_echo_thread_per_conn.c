/*
 * examples/02_echo_thread_per_conn.c - The obvious fix that does not scale.
 *
 * Learning goal: why thread-per-connection fails. Every client gets its own
 * thread with its own kernel stack (often 8 MiB of virtual address space) and
 * its own scheduling cost. Hold 10,000 connections open and watch memory and
 * the scheduler suffer:
 *
 *   gcc -std=c11 -Wall -Wextra -O2 -pthread -o build/02_echo_thread_per_conn
 *       examples/02_echo_thread_per_conn.c
 *   ./build/02_echo_thread_per_conn &
 *   # from another shell, hold connections open:
 *   for i in $(seq 1 2000); do nc 127.0.0.1 9001 </dev/null & done
 *   # The process name is truncated to 15 characters in /proc, so `pidof`
 *   # and `pgrep` by name do not find it; take the PID from the listening
 *   # socket instead (awk avoids sed's backslashes inside a C comment):
 *   pid=$(ss -ltnp | awk -F'pid=' '/:9001 /{split($2,a,","); print a[1]; exit}')
 *   ls /proc/$pid/task | wc -l
 *   grep VmRSS /proc/$pid/status
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9001
#define BACKLOG 128

/* One thread per client. Each stack costs real memory even while idle. */
static void *echo_thread(void *arg)
{
    int fd = (int)(long)arg;
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;

        while (off < n) {
            ssize_t sent = write(fd, buf + off, (size_t)(n - off));

            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                goto done;
            }
            if (sent == 0) {
                /* No progress and no error: retrying would spin forever. */
                goto done;
            }
            off += sent;
        }
    }
done:
    close(fd);
    return NULL;
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
    printf("listening on port %d (one thread per connection)\n", PORT);

    for (;;) {
        pthread_t tid;
        int conn_fd = accept(listen_fd, NULL, NULL);

        if (conn_fd < 0) {
            /* Transient failures (ECONNABORTED, EMFILE, EPROTO) drop one
             * connection instead of taking the example down. */
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO ||
                errno == EMFILE || errno == ENFILE) {
                continue;
            }
            perror("accept");
            return 1;
        }
        if (pthread_create(&tid, NULL, echo_thread, (void *)(long)conn_fd) != 0) {
            /* Thread exhaustion is a real failure mode of this design. */
            perror("pthread_create");
            close(conn_fd);
            continue;
        }
        pthread_detach(tid);
    }
}
