#include "fdcount.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>

/*
 * One atomic per kind. _Atomic uint64_t is lock-free on every platform this
 * project targets (Linux x86-64 and aarch64), so an increment is a single
 * instruction and never blocks a worker.
 */
static _Atomic uint64_t g_opened[DX_FD_KINDS];
static _Atomic uint64_t g_closed[DX_FD_KINDS];

static const char *const k_names[DX_FD_KINDS] = {"conn",  "file",   "cache", "listen",
                                                 "epoll", "wakeup", "log",   "misc"};

void dx_fdcount_opened(dx_fd_kind_t kind)
{
    if ((unsigned)kind < (unsigned)DX_FD_KINDS) {
        atomic_fetch_add_explicit(&g_opened[kind], 1u, memory_order_relaxed);
    }
}

void dx_fdcount_closed(dx_fd_kind_t kind)
{
    if ((unsigned)kind < (unsigned)DX_FD_KINDS) {
        atomic_fetch_add_explicit(&g_closed[kind], 1u, memory_order_relaxed);
    }
}

uint64_t dx_fdcount_total(void)
{
    uint64_t total = 0;

    for (unsigned i = 0; i < (unsigned)DX_FD_KINDS; i++) {
        uint64_t o = atomic_load_explicit(&g_opened[i], memory_order_relaxed);
        uint64_t c = atomic_load_explicit(&g_closed[i], memory_order_relaxed);

        /* A close without a matching open would underflow: report 0 rather than
         * a huge number, and let the actual-vs-believed comparison catch it. */
        total += (o > c) ? (o - c) : 0u;
    }
    return total;
}

void dx_fdcount_by_kind(uint64_t *out)
{
    if (out == NULL) {
        return;
    }
    for (unsigned i = 0; i < (unsigned)DX_FD_KINDS; i++) {
        uint64_t o = atomic_load_explicit(&g_opened[i], memory_order_relaxed);
        uint64_t c = atomic_load_explicit(&g_closed[i], memory_order_relaxed);

        out[i] = (o > c) ? (o - c) : 0u;
    }
}

uint64_t dx_fdcount_actual(void)
{
    DIR *d = opendir("/proc/self/fd");
    uint64_t n = 0;
    const struct dirent *e;

    if (d == NULL) {
        return (uint64_t)-1;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    (void)closedir(d);
    /*
     * The directory descriptor used to enumerate /proc/self/fd is itself
     * listed, so the raw count is always one higher than the truth. The status
     * endpoint publishes this next to "fds_believed" as a comparable value, so
     * the offset is a permanent false positive in the one diagnostic this
     * module exists to provide. Subtract it.
     */
    return (n > 0u) ? n - 1u : 0u;
}

size_t dx_fdcount_json(char *out, size_t cap)
{
    uint64_t by_kind[DX_FD_KINDS];
    int n;

    if (out == NULL || cap == 0u) {
        return 0;
    }
    dx_fdcount_by_kind(by_kind);
    n = snprintf(
        out, cap,
        "\"fds_believed\":%llu,\"fds_actual\":%llu,"
        "\"fds_by_kind\":{\"conn\":%llu,\"file\":%llu,\"cache\":%llu,"
        "\"listen\":%llu,\"epoll\":%llu,\"wakeup\":%llu,\"log\":%llu,\"misc\":%llu}",
        (unsigned long long)dx_fdcount_total(), (unsigned long long)dx_fdcount_actual(),
        (unsigned long long)by_kind[DX_FD_CONN], (unsigned long long)by_kind[DX_FD_FILE],
        (unsigned long long)by_kind[DX_FD_CACHE], (unsigned long long)by_kind[DX_FD_LISTEN],
        (unsigned long long)by_kind[DX_FD_EPOLL], (unsigned long long)by_kind[DX_FD_WAKEUP],
        (unsigned long long)by_kind[DX_FD_LOG], (unsigned long long)by_kind[DX_FD_MISC]);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}

const char *dx_fdcount_kind_name(dx_fd_kind_t kind)
{
    return ((unsigned)kind < (unsigned)DX_FD_KINDS) ? k_names[kind] : "?";
}
