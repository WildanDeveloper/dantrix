#include "util.h"

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int dx_set_affinity(unsigned cpu)
{
    cpu_set_t set;

    if (cpu >= (unsigned)CPU_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

unsigned dx_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return (n > 0) ? (unsigned)n : 1u;
}

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

int dx_send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);

        if (n > 0) {
            p += (size_t)n;
            remaining -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        return -1;
    }
    return 0;
}

bool dx_parse_ull(const char *s, size_t len, unsigned long long *out)
{
    unsigned long long value = 0;

    if (s == NULL || out == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < '0' || c > '9') {
            return false;
        }
        if (value > (ULLONG_MAX - (unsigned long long)(c - '0')) / 10ULL) {
            return false; /* checked multiply-add: never wrap */
        }
        value = value * 10ULL + (unsigned long long)(c - '0');
    }
    *out = value;
    return true;
}

bool dx_parse_size(const char *s, size_t *out)
{
    unsigned long long value = 0;
    size_t i = 0;

    if (s == NULL || out == NULL || s[0] == '\0') {
        return false;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        unsigned char c = (unsigned char)s[i];

        if (value > (ULLONG_MAX - (unsigned long long)(c - '0')) / 10ULL) {
            return false;
        }
        value = value * 10ULL + (unsigned long long)(c - '0');
        i++;
    }
    if (i == 0) {
        return false; /* no digits at all */
    }
    if (s[i] != '\0') {
        /* Exactly one optional binary suffix, nothing after it. */
        if (s[i + 1] != '\0') {
            return false;
        }
        {
            unsigned long long mult;

            switch (s[i]) {
            case 'k':
            case 'K':
                mult = 1024ULL;
                break;
            case 'm':
            case 'M':
                mult = 1024ULL * 1024ULL;
                break;
            case 'g':
            case 'G':
                mult = 1024ULL * 1024ULL * 1024ULL;
                break;
            default:
                return false;
            }
            if (value > ULLONG_MAX / mult) {
                return false;
            }
            value *= mult;
        }
    }
    if (value > SIZE_MAX) {
        return false;
    }
    *out = (size_t)value;
    return true;
}

size_t dx_http_date(char *out, size_t cap, long long when_ms)
{
    struct timespec ts;
    struct tm tm_buf;
    time_t when;

    if (out == NULL || cap < 30u) {
        /* The shortest IMF-fixdate is 29 characters plus the terminator. */
        return 0;
    }
    if (when_ms < 0) {
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            return 0;
        }
        when = ts.tv_sec;
    } else {
        when = (time_t)(when_ms / 1000LL);
    }
    if (gmtime_r(&when, &tm_buf) == NULL) {
        return 0;
    }
    return strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
}

/*
 * Cache for the "Date" response header.
 *
 * Every response carries a Date header, and formatting one costs a gmtime_r()
 * plus a strftime() per response. The value only changes once per second, so it
 * is rendered once per second and copied out from then on: a memcpy instead of
 * a time conversion in the hot path.
 *
 * The cache is thread-local rather than mutex-guarded: a shared buffer would
 * serialise every worker on a single lock, which costs more than the
 * conversion it avoids. Each thread renders its own copy once per second.
 * The cache is only ever written by one thread, so a caller can never observe
 * a half-written value.
 */
static _Thread_local char g_date_cache[40]; /* 29 chars + terminator, padded */
static _Thread_local time_t g_date_cache_sec = -1;

size_t dx_http_date_cached(char *out, size_t cap)
{
    struct timespec ts;
    size_t len;

    if (out == NULL || cap < 30u) {
        return 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    if (ts.tv_sec != g_date_cache_sec) {
        /* Only cache a value that was actually rendered: a failed render must
         * not poison the cache with a stale or empty string. */
        if (dx_http_date(g_date_cache, sizeof(g_date_cache), (long long)ts.tv_sec * 1000LL) > 0) {
            g_date_cache_sec = ts.tv_sec;
        }
    }
    len = strlen(g_date_cache);
    if (len > 0u && cap > len) {
        memcpy(out, g_date_cache, len + 1u);
        return len;
    }
    return 0;
}
