#include "log.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

const char *dx_log_level_name(dx_log_level_t level)
{
    switch (level) {
    case DX_LOG_DEBUG:
        return "debug";
    case DX_LOG_INFO:
        return "info";
    case DX_LOG_WARN:
        return "warn";
    case DX_LOG_ERROR:
        return "error";
    default:
        break;
    }
    return "unknown";
}

bool dx_log_level_parse(const char *name, dx_log_level_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "debug") == 0) {
        *out = DX_LOG_DEBUG;
    } else if (strcmp(name, "info") == 0) {
        *out = DX_LOG_INFO;
    } else if (strcmp(name, "warn") == 0) {
        *out = DX_LOG_WARN;
    } else if (strcmp(name, "error") == 0) {
        *out = DX_LOG_ERROR;
    } else {
        return false;
    }
    return true;
}

void dx_log_write(dx_log_t *log, dx_log_level_t level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm_buf;
    char stamp[40];
    va_list ap;

    if (log == NULL || log->out == NULL || fmt == NULL) {
        return;
    }
    /* Records below the configured level are dropped before formatting. */
    if (level < log->level) {
        return;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        stamp[0] = '\0';
    } else if (gmtime_r(&ts.tv_sec, &tm_buf) == NULL) {
        stamp[0] = '\0';
    } else {
        size_t n = strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        if (n == 0) {
            stamp[0] = '\0';
        } else {
            int written =
                snprintf(stamp + n, sizeof(stamp) - n, ".%03ldZ", (long)(ts.tv_nsec / 1000000L));
            if (written < 0 || (size_t)written >= sizeof(stamp) - n) {
                /* Truncated timestamp: fall back to the seconds-only form. */
                stamp[n] = '\0';
            }
        }
    }

    /*
     * A failed log write is deliberately ignored: the logger must never turn
     * a recoverable problem into a fatal one, and it must not use itself.
     */
    (void)fprintf(log->out, "%s [%s] ", stamp, dx_log_level_name(level));
    va_start(ap, fmt);
    (void)vfprintf(log->out, fmt, ap);
    va_end(ap);
    (void)fputc('\n', log->out);
    (void)fflush(log->out);
}
