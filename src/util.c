#include "util.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

size_t dx_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = 0;

    if (src == NULL) {
        if (size > 0) {
            dst[0] = '\0';
        }
        return 0;
    }

    while (len + 1 < size && src[len] != '\0') {
        dst[len] = src[len];
        len++;
    }
    if (size > 0) {
        dst[len] = '\0';
    }
    /* Advance to the NUL of src so the caller can detect truncation. */
    while (src[len] != '\0') {
        len++;
    }
    return len;
}

size_t dx_strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = 0;

    if (dst == NULL || src == NULL) {
        return 0;
    }

    while (dlen < size && dst[dlen] != '\0') {
        dlen++;
    }
    if (dlen == size) {
        /* Unterminated destination: nothing can be appended safely. */
        return size + strlen(src);
    }

    return dlen + dx_strlcpy(dst + dlen, src, size - dlen);
}

bool dx_parse_int(const char *s, int min, int max, int *out)
{
    bool negative = false;
    long value = 0;
    size_t i = 0;

    if (s == NULL || out == NULL) {
        return false;
    }
    if (s[0] == '\0') {
        return false;
    }
    if (s[0] == '-') {
        negative = true;
        i = 1;
        /* "-0" and "-" are rejected: an explicit sign must precede digits. */
        if (s[i] == '\0') {
            return false;
        }
    }

    for (; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];

        /* Reject anything that is not an ASCII digit, including spaces. */
        if (c < '0' || c > '9') {
            return false;
        }
        /* Checked multiply/add: never rely on wraparound of a signed type. */
        if (value > (LONG_MAX - (long)(c - '0')) / 10L) {
            return false;
        }
        value = value * 10L + (long)(c - '0');
    }

    if (negative) {
        /* "-0" and "-00" carry a meaningless sign; strict parsing rejects them. */
        if (value == 0) {
            return false;
        }
        value = -value;
    }
    if (value < (long)min || value > (long)max) {
        return false;
    }

    *out = (int)value;
    return true;
}

long long dx_now_ms(void)
{
    struct timespec ts;

    /*
     * CLOCK_MONOTONIC is guaranteed to be available on every Linux version we
     * support; the defensive branch keeps the return value well defined even
     * if a future platform only offers the realtime clock.
     */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            return 0;
        }
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

int dx_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);

        if (n > 0) {
            p += (size_t)n;
            remaining -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0) {
            /* No progress and no error: treat as a closed descriptor. */
            errno = EIO;
            return -1;
        }
        return -1;
    }
    return 0;
}
