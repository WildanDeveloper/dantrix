/*
 * log.h - Leveled logger writing to a FILE stream.
 *
 * The logger is deliberately simple for v0.1: synchronous, single stream,
 * one line per record with a UTC timestamp and a level tag. Per-worker
 * buffered logging with rotation arrives in a later milestone; this module
 * only grows its API then, its invariants (never abort on log failure, never
 * recurse) stay.
 */
#ifndef DX_LOG_H
#define DX_LOG_H

#include <stdbool.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define DX_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define DX_PRINTF(fmt_idx, arg_idx)
#endif

typedef enum {
    DX_LOG_DEBUG = 0,
    DX_LOG_INFO = 1,
    DX_LOG_WARN = 2,
    DX_LOG_ERROR = 3
} dx_log_level_t;

typedef struct {
    FILE *out; /* borrowed: this module never fclose()s it */
    dx_log_level_t level;
} dx_log_t;

/* Human readable level tag, e.g. "info". Never returns NULL. */
const char *dx_log_level_name(dx_log_level_t level);

/* Parse a level name ("debug", "info", "warn", "error"); false if unknown. */
bool dx_log_level_parse(const char *name, dx_log_level_t *out);

/*
 * Emit one record. out may be NULL, in which case nothing is printed.
 * fmt must never be data controlled by a remote peer (format string rule).
 */
void dx_log_write(dx_log_t *log, dx_log_level_t level, const char *fmt, ...) DX_PRINTF(3, 4);

/* --------------------------- Access log -------------------------------- */

typedef enum { DX_LOG_FMT_COMMON = 0, DX_LOG_FMT_COMBINED = 1 } dx_log_format_t;

/*
 * Emit one access log line for a completed request.
 *
 * Every client controlled string (peer, method, target, referer, user agent)
 * is escaped: control characters, quotes and backslashes become \xHH so a
 * client cannot forge log lines. referer and user_agent may be NULL (combined
 * format prints "-" for missing values; they are ignored by common).
 */
void dx_log_access(dx_log_t *log, dx_log_format_t fmt, const char *peer, const char *method,
                   size_t method_len, const char *target, size_t target_len, const char *protocol,
                   int status, size_t body_bytes, const char *referer, const char *user_agent);

#endif /* DX_LOG_H */
