/*
 * fdcount.h - Runtime accounting for the descriptors this process owns.
 *
 * The server had a leak that no test caught: a keep-alive connection held one
 * descriptor per request, because the descriptor was released only when the
 * connection closed. Nothing asserted the count, so it passed every functional
 * test and would have exhausted RLIMIT_NOFILE in production.
 *
 * This module makes that class of bug observable two ways:
 *
 *   * Every descriptor the server opens is counted under a short tag, and every
 *     close is counted too. The difference is what the server *believes* is
 *     open, per tag.
 *   * dx_fdcount_actual() counts what the *kernel* has in /proc/self/fd. If the
 *     two disagree, descriptors are being opened or closed outside the tracked
 *     paths -- which is exactly what a leak looks like.
 *
 * The counters are atomic, so workers can close descriptors concurrently
 * without a lock. Cost is one atomic add per open or close: nothing on the
 * parsing or sendfile path.
 */
#ifndef DX_FDCOUNT_H
#define DX_FDCOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One entry per tracked descriptor kind. Keep the list short and stable. */
typedef enum {
    DX_FD_CONN = 0, /* accepted client sockets */
    DX_FD_FILE,     /* served files (owned, not from the cache) */
    DX_FD_CACHE,    /* descriptors held by the open-file cache */
    DX_FD_LISTEN,   /* listener(s) */
    DX_FD_EPOLL,    /* epoll instances */
    DX_FD_WAKEUP,   /* signalfd / eventfd control channels */
    DX_FD_LOG,      /* log files */
    DX_FD_MISC,     /* pid file, /dev/null, mime tables, ... */
    DX_FD_KINDS
} dx_fd_kind_t;

/*
 * Count a descriptor as opened (kind) or closed. These are the only two calls
 * on the hot path: call dx_fdcount_opened() right after a successful
 * open/accept/dup, and dx_fdcount_closed() right after a successful close.
 */
void dx_fdcount_opened(dx_fd_kind_t kind);
void dx_fdcount_closed(dx_fd_kind_t kind);

/*
 * Descriptors the server believes it has open, per kind, plus the total.
 * out may be NULL to get only the total.
 */
uint64_t dx_fdcount_total(void);
void dx_fdcount_by_kind(uint64_t *out);

/*
 * What the kernel reports in /proc/self/fd, or (uint64_t)-1 when /proc is not
 * mounted. Note that /proc/self/fd includes the descriptor used to enumerate
 * the directory, so the value is one higher than the truth on Linux; callers
 * compare deltas, so the constant offset does not matter.
 */
uint64_t dx_fdcount_actual(void);

/*
 * Snapshot of the accounting as JSON-ish fields for the status endpoint.
 * Returns the number of bytes written, or 0 when the buffer is too small.
 */
size_t dx_fdcount_json(char *out, size_t cap);

/* Human-readable names, in dx_fd_kind_t order. */
const char *dx_fdcount_kind_name(dx_fd_kind_t kind);

#endif /* DX_FDCOUNT_H */
