/*
 * util.h - Small dependency-free helpers (strings, numbers, time, I/O).
 *
 * All functions here are safe to call from any module. None of them log:
 * logging from a helper would recurse into the logger on error paths.
 */
#ifndef DX_UTIL_H
#define DX_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bounded string copy, always NUL terminating when size > 0.
 * Returns the length of src, so truncation is detectable (>= size).
 */
size_t dx_strlcpy(char *dst, const char *src, size_t size);

/*
 * Bounded string append, always NUL terminating when size > 0.
 * Returns the length the result would have had.
 */
size_t dx_strlcat(char *dst, const char *src, size_t size);

/*
 * Strict decimal integer parser. Accepts optional leading '-' followed by one
 * or more digits and nothing else: no whitespace, no '+', no "0x", no trailing
 * garbage. Rejects overflow and values outside [min, max].
 * Returns true on success.
 */
bool dx_parse_int(const char *s, int min, int max, int *out);

/* Monotonic milliseconds since an unspecified origin. Never goes backwards. */
long long dx_now_ms(void);

/*
 * Pin the calling thread to a single CPU (the "i-th worker to the i-th core"
 * rule). Returns 0 on success, -1 with errno set (e.g. EINVAL when cpu is not
 * an online CPU). The caller decides whether a failure is fatal.
 */
int dx_set_affinity(unsigned cpu);

/*
 * Number of CPUs available to this process (>= 1). Used to resolve
 * "--workers 0" (auto). Never returns 0, so callers can use it directly.
 */
unsigned dx_cpu_count(void);

/*
 * Write exactly len bytes to fd, retrying on EINTR and short writes.
 * Returns 0 on success, -1 on error (errno preserved).
 */
int dx_write_all(int fd, const void *buf, size_t len);

/*
 * send() wrapper with the same guarantees as dx_write_all(), always using
 * MSG_NOSIGNAL so a disconnected peer cannot raise SIGPIPE.
 * Returns 0 on success, -1 on error (errno preserved).
 */
int dx_send_all(int fd, const void *buf, size_t len);

/*
 * Strict unsigned decimal parser over an exact byte range (no NUL needed).
 * Rejects empty input, non-digits, and values that overflow unsigned long long.
 * Returns true on success.
 */
bool dx_parse_ull(const char *s, size_t len, unsigned long long *out);

/*
 * Parse a size with an optional binary suffix: "8k", "1m", "2g" (base 1024)
 * or a plain byte count. Rejects empty input, garbage, and overflow.
 * Returns true on success.
 */
bool dx_parse_size(const char *s, size_t *out);

/*
 * Format an IMF-fixdate (RFC 9110 5.6.7), e.g.
 * "Sun, 11 Oct 2026 03:15:30 GMT". when_ms may be -1 for the current time.
 * Writes at most cap bytes; always NUL terminates when cap > 0.
 * Returns the number of bytes written excluding the terminator, or 0 on
 * failure.
 */
size_t dx_http_date(char *out, size_t cap, long long when_ms);

#endif /* DX_UTIL_H */
