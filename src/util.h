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
 * Write exactly len bytes to fd, retrying on EINTR and short writes.
 * Returns 0 on success, -1 on error (errno preserved).
 */
int dx_write_all(int fd, const void *buf, size_t len);

#endif /* DX_UTIL_H */
