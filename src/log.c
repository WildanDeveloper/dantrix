#include "log.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

void dx_log_init(dx_log_t *log, FILE *out, dx_log_level_t level)
{
    if (log == NULL) {
        return;
    }
    memset(log, 0, sizeof(*log));
    log->out = out;
    log->level = level;
    if (pthread_mutex_init(&log->mutex, NULL) == 0) {
        log->mutex_ready = true;
    }
    /* A failed init leaves mutex_ready false: logging degrades to unlocked. */
}

void dx_log_destroy(dx_log_t *log)
{
    if (log == NULL || !log->mutex_ready) {
        return;
    }
    pthread_mutex_destroy(&log->mutex);
    log->mutex_ready = false;
}

/* Serialize one record when the logger was prepared for concurrent use. */
static void log_lock(const dx_log_t *log)
{
    if (log != NULL && log->mutex_ready) {
        (void)pthread_mutex_lock((pthread_mutex_t *)&log->mutex);
    }
}

static void log_unlock(const dx_log_t *log)
{
    if (log != NULL && log->mutex_ready) {
        (void)pthread_mutex_unlock((pthread_mutex_t *)&log->mutex);
    }
}

void dx_log_reopen(dx_log_t *log, FILE *new_out)
{
    if (log == NULL) {
        return;
    }
    log_lock(log);
    log->out = new_out;
    log_unlock(log);
}

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

    if (log == NULL || fmt == NULL) {
        return;
    }
    /*
     * log->out is read under the mutex: the SIGUSR1 reopen path swaps it and
     * closes the old stream, so an unlocked read here could race a reopen and
     * write to a closed FILE*. Records below the configured level are dropped
     * first -- log->level is set once before the threads exist.
     */
    if (level < log->level) {
        return;
    }
    log_lock(log);
    if (log->out == NULL) {
        log_unlock(log);
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
    log_unlock(log);
}

/* --------------------------- Access log -------------------------------- */

/*
 * Append s to out at *pos, escaping control characters, quotes and backslashes
 * as \xHH. Returns false when the output would overflow cap (the value is then
 * truncated earlier by the caller's bound anyway; the flag stops further
 * writes).
 */
static bool append_escaped(char *out, size_t cap, size_t *pos, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20u || c >= 0x7fu || c == '"' || c == '\\') {
            if (*pos + 4u >= cap) {
                return false;
            }
            out[(*pos)++] = '\\';
            out[(*pos)++] = 'x';
            out[(*pos)++] = hex[(c >> 4) & 0xfu];
            out[(*pos)++] = hex[c & 0xfu];
        } else {
            if (*pos + 1u >= cap) {
                return false;
            }
            out[(*pos)++] = (char)c;
        }
    }
    return true;
}

/* Append fixed structural text (separators, delimiters) without escaping. */
static void append_raw(char *out, size_t cap, size_t *pos, const char *s)
{
    while (*pos + 1u < cap && *s != '\0') {
        out[(*pos)++] = *s++;
    }
}

/*
 * Append a JSON string value (without the surrounding quotes) at *pos.
 *
 * The access-log escaper writes \xHH for control bytes, which is not valid
 * JSON: a log parser reading a json-format line would reject it. JSON needs
 * \" \\ \b \f \n \r \t and \u00XX for the remaining control characters, and a
 * UTF-8 byte sequence passes through unchanged (JSON strings are Unicode, and
 * a well-formed UTF-8 stream is valid inside them). Bytes >= 0x80 are copied
 * as-is for that reason; the peer cannot inject a raw control byte because
 * every byte below 0x20 is escaped.
 */
static void append_json_string(char *out, size_t cap, size_t *pos, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') {
            if (*pos + 2u >= cap) {
                return;
            }
            out[(*pos)++] = '\\';
            out[(*pos)++] = (char)c;
        } else if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            const char esc = c == '\b'   ? 'b'
                             : c == '\f' ? 'f'
                             : c == '\n' ? 'n'
                             : c == '\r' ? 'r'
                                         : 't';

            if (*pos + 2u >= cap) {
                return;
            }
            out[(*pos)++] = '\\';
            out[(*pos)++] = esc;
        } else if (c < 0x20u) {
            if (*pos + 6u >= cap) {
                return;
            }
            out[(*pos)++] = '\\';
            out[(*pos)++] = 'u';
            out[(*pos)++] = '0';
            out[(*pos)++] = '0';
            out[(*pos)++] = hex[(c >> 4) & 0xfu];
            out[(*pos)++] = hex[c & 0xfu];
        } else {
            if (*pos + 1u >= cap) {
                return;
            }
            out[(*pos)++] = (char)c;
        }
    }
}

/* Append `"key":"value"` with the value JSON-escaped. */
static void append_json_field(char *out, size_t cap, size_t *pos, const char *key,
                              const char *value, size_t len, bool *first)
{
    if (!*first) {
        append_raw(out, cap, pos, ",");
    }
    *first = false;
    append_raw(out, cap, pos, "\"");
    append_raw(out, cap, pos, key);
    append_raw(out, cap, pos, "\":\"");
    append_json_string(out, cap, pos, value, len);
    append_raw(out, cap, pos, "\"");
}

/* Append `"key":<number>`; the caller formats the number. */
static void append_json_number(char *out, size_t cap, size_t *pos, const char *key,
                               const char *number, bool *first)
{
    if (!*first) {
        append_raw(out, cap, pos, ",");
    }
    *first = false;
    append_raw(out, cap, pos, "\"");
    append_raw(out, cap, pos, key);
    append_raw(out, cap, pos, "\":");
    append_raw(out, cap, pos, number);
}

void dx_log_access(dx_log_t *log, dx_log_format_t fmt, const char *peer, const char *method,
                   size_t method_len, const char *target, size_t target_len, const char *protocol,
                   int status, size_t body_bytes, const char *referer, const char *user_agent)
{
    struct timespec ts;
    struct tm tm_buf;
    char stamp[64];
    char line[2048];
    size_t pos = 0;

    if (log == NULL) {
        return;
    }
    /*
     * log->out is read under the mutex for the same reason as in
     * dx_log_write(): the SIGUSR1 reopen path swaps it and closes the old
     * stream, so an unlocked read here could race a reopen and write to a
     * closed FILE*. A NULL out is a documented no-op.
     */
    log_lock(log);
    if (log->out == NULL) {
        log_unlock(log);
        return;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || localtime_r(&ts.tv_sec, &tm_buf) == NULL) {
        stamp[0] = '\0';
    } else if (strftime(stamp, sizeof(stamp), "%d/%b/%Y:%H:%M:%S %z", &tm_buf) == 0) {
        stamp[0] = '\0';
    }

#define ACC_APPEND(s)                                                                              \
    do {                                                                                           \
        const char *str_ = (s);                                                                    \
        append_escaped(line, sizeof(line), &pos, str_, strlen(str_));                              \
    } while (0)

    if (fmt == DX_LOG_FMT_JSON) {
        /*
         * One JSON object per line, and nothing else on the line: this branch
         * must run before the common/combined fields are appended, or every
         * line would carry both. The field names are a promise (a parser keys
         * off them): ts, peer, method, target, protocol, status, bytes,
         * referer, user_agent.
         */
        bool first = true;
        char num[32];
        int n;

        append_raw(line, sizeof(line), &pos, "{");
        append_json_field(line, sizeof(line), &pos, "ts", stamp[0] != '\0' ? stamp : "-",
                          strlen(stamp[0] != '\0' ? stamp : "-"), &first);
        append_json_field(line, sizeof(line), &pos, "peer", peer != NULL ? peer : "-",
                          strlen(peer != NULL ? peer : "-"), &first);
        append_json_field(line, sizeof(line), &pos, "method",
                          method != NULL && method_len > 0 ? method : "-",
                          method != NULL && method_len > 0 ? method_len : 1u, &first);
        append_json_field(line, sizeof(line), &pos, "target",
                          target != NULL && target_len > 0 ? target : "-",
                          target != NULL && target_len > 0 ? target_len : 1u, &first);
        append_json_field(line, sizeof(line), &pos, "protocol",
                          protocol != NULL ? protocol : "HTTP/1.1",
                          strlen(protocol != NULL ? protocol : "HTTP/1.1"), &first);
        n = snprintf(num, sizeof(num), "%d", status);
        if (n > 0) {
            append_json_number(line, sizeof(line), &pos, "status", num, &first);
        }
        n = snprintf(num, sizeof(num), "%zu", body_bytes);
        if (n > 0) {
            append_json_number(line, sizeof(line), &pos, "bytes", num, &first);
        }
        append_json_field(line, sizeof(line), &pos, "referer",
                          referer != NULL && referer[0] != '\0' ? referer : "-",
                          strlen(referer != NULL && referer[0] != '\0' ? referer : "-"), &first);
        append_json_field(line, sizeof(line), &pos, "user_agent",
                          user_agent != NULL && user_agent[0] != '\0' ? user_agent : "-",
                          strlen(user_agent != NULL && user_agent[0] != '\0' ? user_agent : "-"),
                          &first);
        append_raw(line, sizeof(line), &pos, "}");

        if (pos + 2u < sizeof(line)) {
            line[pos++] = '\n';
            line[pos] = '\0';
            (void)fwrite(line, 1, pos, log->out);
            (void)fflush(log->out);
        }
        log_unlock(log);
        return;
    }

    ACC_APPEND(peer != NULL ? peer : "-");
    append_raw(line, sizeof(line), &pos, " - - [");
    ACC_APPEND(stamp[0] != '\0' ? stamp : "-");
    append_raw(line, sizeof(line), &pos, "] \"");
    if (method != NULL && method_len > 0) {
        append_escaped(line, sizeof(line), &pos, method, method_len);
    } else {
        append_escaped(line, sizeof(line), &pos, "-", 1u);
    }
    append_raw(line, sizeof(line), &pos, " ");
    if (target != NULL && target_len > 0) {
        append_escaped(line, sizeof(line), &pos, target, target_len);
    } else {
        append_escaped(line, sizeof(line), &pos, "-", 1u);
    }
    append_raw(line, sizeof(line), &pos, " ");
    ACC_APPEND(protocol != NULL ? protocol : "HTTP/1.1");
    append_raw(line, sizeof(line), &pos, "\" ");

    /* Status and body bytes: numbers, appended without stdio. */
    {
        char num[16];
        int n = snprintf(num, sizeof(num), "%d %zu", status, body_bytes);

        if (n > 0) {
            append_raw(line, sizeof(line), &pos, num);
        }
    }

    if (fmt == DX_LOG_FMT_COMBINED) {
        append_raw(line, sizeof(line), &pos, " \"");
        ACC_APPEND(referer != NULL && referer[0] != '\0' ? referer : "-");
        append_raw(line, sizeof(line), &pos, "\" \"");
        ACC_APPEND(user_agent != NULL && user_agent[0] != '\0' ? user_agent : "-");
        append_raw(line, sizeof(line), &pos, "\"");
    }

    if (pos + 2u < sizeof(line)) {
        line[pos++] = '\n';
        line[pos] = '\0';
        /* The mutex is already held (taken at the top of the function). */
        (void)fwrite(line, 1, pos, log->out);
        (void)fflush(log->out);
        log_unlock(log);
    }
#undef ACC_APPEND
}
