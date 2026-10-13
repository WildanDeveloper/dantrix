/*
 * seccomp.h - Optional seccomp-bpf sandbox.
 *
 * The server only ever needs a small, fixed set of system calls: sockets,
 * epoll, files below the document root, and the log. Anything else is either a
 * bug or an attempt to turn a request into code execution. A seccomp filter
 * installed after start-up makes that a hard guarantee: a syscall outside the
 * allow list kills the process instead of being carried out.
 *
 * The filter is built from a table of syscall numbers that was collected by
 * tracing the server through every feature (static files, listings, ranges,
 * conditional requests, the status endpoint, reload, log reopen, statistics and
 * a graceful shutdown) and taking the distinct set. `make test-seccomp` replays
 * that workload with the filter armed and fails if the server dies, so the list
 * cannot silently go stale.
 *
 * Off by default: it is a hardening option, and it changes behaviour under a
 * crash (SIGSYS instead of an error return), so it is opt-in.
 */
#ifndef DX_SECCOMP_H
#define DX_SECCOMP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Install the filter on the calling thread and every thread created later
 * (SECCOMP_FILTER_FLAG_TSYNC is used when the caller is already multi-threaded,
 * so the whole process is covered).
 *
 * Returns 0 on success. Returns -1 with errno set when seccomp is unavailable,
 * which the caller reports as a warning rather than a fatal error: a kernel
 * without CONFIG_SECCOMP is still able to serve. errno is ENOSYS when the
 * kernel has no seccomp, EPERM when a policy forbids installing a filter, and
 * EINVAL when the kernel refuses the filter itself.
 */
int dx_seccomp_apply(void);

/* True when the running kernel advertises seccomp support. */
bool dx_seccomp_available(void);

/*
 * The syscall allow list, for the tests and for `--print-config`. The array is
 * NULL-terminated by count == *out_count.
 */
const int *dx_seccomp_allowlist(size_t *out_count);

/* Names matching the allow list, for diagnostics. Same length as the list. */
const char *const *dx_seccomp_allowlist_names(size_t *out_count);

#endif /* DX_SECCOMP_H */
