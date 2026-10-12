#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dantrix.h"
#include "http_parser.h"
#include "net.h"
#include "util.h"

/* ----------------------------- key registry ---------------------------- */

typedef enum {
    K_BOOL,
    K_INT,
    K_SIZE,
    K_DUR,
    K_STR,
    K_PATH,
    K_LOGLEVEL,
    K_LOGFORMAT,
    K_ADDHEADER
} key_kind_t;

typedef struct {
    const char *name;
    key_kind_t kind;
    size_t offset; /* offsetof(dx_settings_t, field) */
    size_t size;   /* buffer size for K_STR/K_PATH */
    long long min; /* inclusive numeric bound (K_INT/K_SIZE/K_DUR) */
    long long max;
} dx_key_t;

#define K(name, kind, field, min, max) {name, kind, offsetof(dx_settings_t, field), 0u, min, max}

/* String-valued keys carry their buffer size. */
#define KS(name, kind, field, min, max)                                                            \
    {name, kind, offsetof(dx_settings_t, field), sizeof(((dx_settings_t *)0)->field), min, max}

/* add_header has no single field: it is a repeated array handled specially. */
#define K_ADD(name) {name, K_ADDHEADER, 0u, 0u, 0, 0}

static const dx_key_t dx_keys[] = {
    KS("bind", K_STR, bind, 0, 0),
    K("port", K_INT, port, 1, 65535),
    K("workers", K_INT, workers, 0, 1024),
    K("backlog", K_INT, backlog, 1, 65535),
    K("reuseport", K_BOOL, reuseport, 0, 0),
    K("max_connections", K_SIZE, max_connections, 1, 1000000),
    K("cpu_affinity", K_BOOL, cpu_affinity, 0, 0),
    K("edge_triggered", K_BOOL, edge_triggered, 0, 0),

    K("header_timeout", K_DUR, header_timeout_ms, 1, 86400000),
    K("body_timeout", K_DUR, body_timeout_ms, 1, 86400000),
    K("send_timeout", K_DUR, send_timeout_ms, 1, 86400000),
    K("keepalive_timeout", K_DUR, keepalive_timeout_ms, 0, 86400000),
    K("shutdown_timeout", K_DUR, shutdown_timeout_ms, 0, 86400000),

    K("keepalive_requests", K_SIZE, keepalive_requests, 1, 10000000),
    K("max_request_line", K_SIZE, max_request_line, 64, 1048576),
    K("max_header_size", K_SIZE, max_header_size, 256, 16777216),
    K("max_headers", K_SIZE, max_headers, 1, DX_MAX_HEADERS_LIMIT),
    K("max_body_size", K_SIZE, max_body_size, 0, 1LL << 40),

    KS("root", K_PATH, root, 0, 0),
    KS("index", K_STR, index, 0, 0),
    K("directory_listing", K_BOOL, directory_listing, 0, 0),
    K("follow_symlinks", K_BOOL, follow_symlinks, 0, 0),
    K("hidden_files", K_BOOL, hidden_files, 0, 0),
    K("sendfile", K_BOOL, sendfile, 0, 0),
    K("sendfile_chunk", K_SIZE, sendfile_chunk, 4096, 67108864),
    K("etag", K_BOOL, etag, 0, 0),
    KS("mime_types", K_PATH, mime_types, 0, 0),
    KS("default_type", K_STR, default_type, 0, 0),
    KS("cache_control", K_STR, cache_control, 0, 0),
    K("server_tokens", K_BOOL, server_tokens, 0, 0),
    K_ADD("add_header"),

    K("tcp_nodelay", K_BOOL, tcp_nodelay, 0, 0),
    K("tcp_defer_accept", K_DUR, tcp_defer_accept_ms, 0, 86400000),
    K("tcp_fastopen", K_INT, tcp_fastopen, 0, 65535),
    K("socket_rcvbuf", K_SIZE, socket_rcvbuf, 0, 1LL << 30),
    K("socket_sndbuf", K_SIZE, socket_sndbuf, 0, 1LL << 30),

    KS("access_log", K_STR, access_log, 0, 0),
    KS("error_log", K_STR, error_log, 0, 0),
    K("log_level", K_LOGLEVEL, log_level, 0, 0),
    K("log_format", K_LOGFORMAT, log_format, 0, 0),

    K("daemonize", K_BOOL, daemonize, 0, 0),
    KS("pid_file", K_PATH, pid_file, 0, 0),
    KS("user", K_STR, user, 0, 0),
    KS("group", K_STR, group, 0, 0),

    KS("status_path", K_STR, status_path, 0, 0),
};

static const size_t dx_key_count = sizeof(dx_keys) / sizeof(dx_keys[0]);

size_t dx_config_key_count(void)
{
    return dx_key_count;
}

static const dx_key_t *key_lookup(const char *name)
{
    size_t i;

    for (i = 0; i < dx_key_count; i++) {
        if (strcmp(dx_keys[i].name, name) == 0) {
            return &dx_keys[i];
        }
    }
    return NULL;
}

/* ------------------------------- defaults ------------------------------ */

void dx_config_defaults(dx_settings_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));

    dx_strlcpy(s->bind, DX_DEFAULT_BIND, sizeof(s->bind));
    s->port = DX_DEFAULT_PORT;
    s->workers = 1; /* 1 = single-threaded, matching the v0.1..v0.5 default */
    s->backlog = DX_DEFAULT_BACKLOG;
    s->reuseport = true;
    s->max_connections = DX_DEFAULT_MAX_CONNS;
    s->cpu_affinity = false;
    s->edge_triggered = false;

    s->header_timeout_ms = 10000u;
    s->body_timeout_ms = 30000u;
    s->send_timeout_ms = 30000u;
    s->keepalive_timeout_ms = 30000u;
    s->shutdown_timeout_ms = 10000u;

    s->keepalive_requests = 1000u;
    s->max_request_line = 8u * 1024u;
    s->max_header_size = 16u * 1024u;
    s->max_headers = 100u;
    s->max_body_size = 1024u * 1024u;

    dx_strlcpy(s->root, DX_DEFAULT_ROOT, sizeof(s->root));
    dx_strlcpy(s->index, DX_DEFAULT_INDEX, sizeof(s->index));
    s->directory_listing = false;
    s->follow_symlinks = false;
    s->hidden_files = false;
    s->sendfile = true;
    s->sendfile_chunk = 1024u * 1024u;
    s->etag = true;
    s->mime_types[0] = '\0';
    dx_strlcpy(s->default_type, DX_DEFAULT_MIME, sizeof(s->default_type));
    s->cache_control[0] = '\0';
    s->server_tokens = true;
    s->add_header_count = 0;

    s->tcp_nodelay = true;
    s->tcp_defer_accept_ms = 0;
    s->tcp_fastopen = 0;
    s->socket_rcvbuf = 0;
    s->socket_sndbuf = 0;

    dx_strlcpy(s->access_log, "off", sizeof(s->access_log));
    dx_strlcpy(s->error_log, "stderr", sizeof(s->error_log));
    s->log_level = DX_LOG_INFO;
    s->log_format = DX_LOG_FMT_COMBINED;

    s->daemonize = false;
    s->pid_file[0] = '\0';
    s->user[0] = '\0';
    s->group[0] = '\0';
    s->status_path[0] = '\0';
}

/* ------------------------------- parsing ------------------------------- */

static bool parse_bool(const char *v, bool *out)
{
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "on") == 0 ||
        strcmp(v, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "off") == 0 ||
        strcmp(v, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/*
 * Duration with the s/m/h suffix (base 1000), plus ms for the sub-second form.
 * A bare number means seconds, matching the configuration file examples.
 */
static bool parse_duration_ms(const char *v, uint64_t *out)
{
    unsigned long long value = 0;
    unsigned long long mult = 1000ULL;
    size_t i = 0;

    if (v == NULL || v[0] == '\0') {
        return false;
    }
    while (v[i] >= '0' && v[i] <= '9') {
        if (value > (ULLONG_MAX - 9ULL) / 10ULL) {
            return false;
        }
        value = value * 10ULL + (unsigned long long)(v[i] - '0');
        i++;
    }
    if (i == 0) {
        return false;
    }
    if (strcmp(v + i, "ms") == 0) {
        mult = 1ULL;
    } else if (v[i] == '\0' || strcmp(v + i, "s") == 0) {
        mult = 1000ULL;
    } else if (strcmp(v + i, "m") == 0) {
        mult = 60000ULL;
    } else if (strcmp(v + i, "h") == 0) {
        mult = 3600000ULL;
    } else {
        return false;
    }
    if (value > UINT64_MAX / mult) {
        return false;
    }
    *out = (uint64_t)value * mult;
    return true;
}

static bool parse_ll(const char *v, long long *out)
{
    unsigned long long u = 0;
    size_t len = strlen(v);

    if (!dx_parse_ull(v, len, &u)) {
        return false;
    }
    if (u > (unsigned long long)LLONG_MAX) {
        return false;
    }
    *out = (long long)u;
    return true;
}

/*
 * Decode a value into the target field. Returns 0 on success, -1 on error
 * with a message on msg. Repeated single-value keys are reported by the caller
 * (this function only writes the value).
 */
static int apply_value(dx_settings_t *s, const dx_key_t *key, const char *value, const char *src,
                       FILE *msg)
{
    unsigned char *base = (unsigned char *)s;
    void *field = base + key->offset;
    long long ll = 0;
    uint64_t dur = 0;
    size_t sz = 0;
    bool b = false;

    switch (key->kind) {
    case K_BOOL:
        if (!parse_bool(value, &b)) {
            fprintf(msg, "dantrix: %s: '%s': expected a boolean\n", src, value);
            return -1;
        }
        *(bool *)field = b;
        return 0;

    case K_INT:
        /*
         * workers accepts "auto" (= one worker per online CPU), stored as 0
         * and resolved to a concrete count by main().
         */
        if (strcmp(key->name, "workers") == 0 && strcmp(value, "auto") == 0) {
            *(int *)field = 0;
            return 0;
        }
        if (!parse_ll(value, &ll) || ll < key->min || ll > key->max) {
            fprintf(msg, "dantrix: %s: '%s': out of range [%lld, %lld]\n", src, value, key->min,
                    key->max);
            return -1;
        }
        *(int *)field = (int)ll;
        return 0;

    case K_SIZE:
        if (!dx_parse_size(value, &sz) || (long long)sz < key->min || (long long)sz > key->max) {
            fprintf(msg, "dantrix: %s: '%s': invalid size\n", src, value);
            return -1;
        }
        *(size_t *)field = sz;
        return 0;

    case K_DUR:
        if (!parse_duration_ms(value, &dur) || (long long)dur < key->min ||
            (long long)dur > key->max) {
            fprintf(msg, "dantrix: %s: '%s': invalid duration\n", src, value);
            return -1;
        }
        *(uint64_t *)field = dur;
        return 0;

    case K_STR:
    case K_PATH: {
        size_t cap = key->size;

        if (dx_strlcpy((char *)field, value, cap) >= cap) {
            fprintf(msg, "dantrix: %s: value too long (max %zu)\n", src, cap - 1u);
            return -1;
        }
        return 0;
    }

    case K_LOGLEVEL:
        if (!dx_log_level_parse(value, (dx_log_level_t *)field)) {
            fprintf(msg, "dantrix: %s: unknown log level '%s'\n", src, value);
            return -1;
        }
        return 0;

    case K_LOGFORMAT:
        if (strcmp(value, "common") == 0) {
            *(dx_log_format_t *)field = DX_LOG_FMT_COMMON;
        } else if (strcmp(value, "combined") == 0) {
            *(dx_log_format_t *)field = DX_LOG_FMT_COMBINED;
        } else {
            fprintf(msg, "dantrix: %s: unknown log format '%s'\n", src, value);
            return -1;
        }
        return 0;

    case K_ADDHEADER:
        if (s->add_header_count >= DX_CFG_MAX_ADD_HEADERS) {
            fprintf(msg, "dantrix: %s: at most %u add_header entries\n", src,
                    (unsigned)DX_CFG_MAX_ADD_HEADERS);
            return -1;
        }
        if (dx_strlcpy(s->add_header[s->add_header_count], value, DX_CFG_STR_MAX) >=
            DX_CFG_STR_MAX) {
            fprintf(msg, "dantrix: %s: add_header value too long\n", src);
            return -1;
        }
        s->add_header_count++;
        return 0;

    default:
        break;
    }
    fprintf(msg, "dantrix: %s: unhandled key type\n", src);
    return -1;
}

/* Apply a key=value pair from any source. seen may be NULL (env/override). */
static int apply_pair(dx_settings_t *s, const char *name, const char *value, const char *src,
                      unsigned char *seen, FILE *msg)
{
    const dx_key_t *key = key_lookup(name);

    if (key == NULL) {
        fprintf(msg, "dantrix: %s: unknown key '%s'\n", src, name);
        return -1;
    }
    if (seen != NULL && key->kind != K_ADDHEADER) {
        size_t idx = (size_t)(key - dx_keys);

        if (seen[idx] != 0u) {
            fprintf(msg, "dantrix: %s: key '%s' repeated, last value wins\n", src, name);
        }
        seen[idx] = 1u;
    }
    return apply_value(s, key, value, src, msg);
}

/* Strip a trailing inline comment (" # ...") and surrounding whitespace. */
static void trim_value(char *v)
{
    size_t len;
    char *hash;

    /* Inline comment: a '#' preceded by whitespace. */
    hash = strstr(v, " #");
    if (hash != NULL) {
        *hash = '\0';
    } else if (v[0] == '#') {
        v[0] = '\0';
    }
    len = strlen(v);
    while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\r')) {
        v[--len] = '\0';
    }
}

/* Unquote a double-quoted value in place, decoding \" and \\. Returns -1 on a
 * missing closing quote. */
static int unquote(char *v, FILE *msg, const char *src)
{
    char *out = v;
    char *p;

    if (v[0] != '"') {
        return 0; /* not quoted: leave as is */
    }
    p = v + 1;
    while (*p != '\0') {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            *out++ = p[1];
            p += 2;
            continue;
        }
        if (*p == '"') {
            *out = '\0';
            return 0;
        }
        *out++ = *p++;
    }
    fprintf(msg, "dantrix: %s: unterminated quoted value\n", src);
    return -1;
}

int dx_config_file(dx_settings_t *s, const char *path, FILE *msg)
{
    FILE *f;
    char line[8192];
    unsigned char seen[sizeof(dx_keys) / sizeof(dx_keys[0])];
    unsigned long lineno = 0;
    int rc = 0;

    if (s == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(msg, "dantrix: cannot open config '%s': %s\n", path, dx_strerror(errno));
        return -1;
    }
    memset(seen, 0, sizeof(seen));

    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq;
        char *name;
        char *value;

        lineno++;
        /* A line longer than the buffer is refused rather than silently split. */
        if (strchr(line, '\n') == NULL && !feof(f)) {
            fprintf(msg, "dantrix: %s:%lu: line too long\n", path, lineno);
            rc = -1;
            break;
        }
        value = strchr(line, '\n');
        if (value != NULL) {
            *value = '\0';
        }
        /* Skip blank lines and full-line comments. */
        name = line;
        while (*name == ' ' || *name == '\t') {
            name++;
        }
        if (*name == '\0' || *name == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            fprintf(msg, "dantrix: %s:%lu: expected key = value\n", path, lineno);
            rc = -1;
            break;
        }
        *eq = '\0';
        name = line;
        value = eq + 1;

        /* Trim leading whitespace of the key, then trailing. */
        while (*name == ' ' || *name == '\t') {
            name++;
        }
        {
            char *end = name + strlen(name);

            while (end > name && (end[-1] == ' ' || end[-1] == '\t')) {
                *--end = '\0';
            }
        }
        /* Trim leading spaces of the value. */
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        if (*value == '"') {
            if (unquote(value, msg, path) != 0) {
                rc = -1;
                break;
            }
        } else {
            trim_value(value);
        }

        {
            char src[300];
            snprintf(src, sizeof(src), "%s:%lu", path, lineno);
            if (apply_pair(s, name, value, src, seen, msg) != 0) {
                rc = -1;
                break;
            }
        }
    }
    fclose(f);
    return rc;
}

int dx_config_env(dx_settings_t *s, FILE *msg)
{
    size_t i;

    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < dx_key_count; i++) {
        char env[256];
        char upper[192];
        const char *name = dx_keys[i].name;
        size_t j;
        const char *value;

        for (j = 0; name[j] != '\0' && j + 1u < sizeof(upper); j++) {
            upper[j] = (char)toupper((unsigned char)name[j]);
        }
        upper[j] = '\0';
        snprintf(env, sizeof(env), "DANTRIX_%s", upper);

        value = getenv(env);
        if (value == NULL) {
            continue;
        }
        if (apply_pair(s, name, value, env, NULL, msg) != 0) {
            return -1;
        }
    }
    return 0;
}

int dx_config_override(dx_settings_t *s, const char *assignment, FILE *msg)
{
    return dx_config_override_src(s, assignment, "-o", msg);
}

int dx_config_override_src(dx_settings_t *s, const char *assignment, const char *src, FILE *msg)
{
    char buf[8192];
    char *eq;

    if (s == NULL || assignment == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (dx_strlcpy(buf, assignment, sizeof(buf)) >= sizeof(buf)) {
        fprintf(msg, "dantrix: %s: assignment too long\n", src);
        return -1;
    }
    eq = strchr(buf, '=');
    if (eq == NULL) {
        fprintf(msg, "dantrix: %s: expected key=value, got '%s'\n", src, assignment);
        return -1;
    }
    *eq = '\0';
    return apply_pair(s, buf, eq + 1, src, NULL, msg);
}

int dx_config_validate(const dx_settings_t *s, FILE *msg)
{
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (s->max_header_size < s->max_request_line) {
        fprintf(msg, "dantrix: max_header_size (%zu) must be >= max_request_line (%zu)\n",
                s->max_header_size, s->max_request_line);
        return -1;
    }
    if (s->sendfile_chunk < 4096u) {
        fprintf(msg, "dantrix: sendfile_chunk must be at least 4096\n");
        return -1;
    }
    return 0;
}

int dx_config_check_root(const dx_settings_t *s, FILE *msg)
{
    struct stat st;

    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stat(s->root, &st) != 0) {
        fprintf(msg, "dantrix: document root '%s': %s\n", s->root, dx_strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(msg, "dantrix: document root '%s' is not a directory\n", s->root);
        return -1;
    }
    if (access(s->root, R_OK | X_OK) != 0) {
        fprintf(msg, "dantrix: document root '%s' is not readable: %s\n", s->root,
                dx_strerror(errno));
        return -1;
    }
    return 0;
}

/* -------------------------------- printing ----------------------------- */

static void print_value(const dx_settings_t *s, const dx_key_t *key, FILE *out)
{
    const unsigned char *base = (const unsigned char *)s;
    const void *field = base + key->offset;

    switch (key->kind) {
    case K_BOOL:
        fprintf(out, "%s", (*(const bool *)field) ? "true" : "false");
        break;
    case K_INT:
        fprintf(out, "%d", *(const int *)field);
        break;
    case K_SIZE:
        fprintf(out, "%zu", *(const size_t *)field);
        break;
    case K_DUR:
        fprintf(out, "%llums", (unsigned long long)(*(const uint64_t *)field));
        break;
    case K_STR:
    case K_PATH:
        fprintf(out, "%s", (const char *)field);
        break;
    case K_LOGLEVEL:
        fprintf(out, "%s", dx_log_level_name(*(const dx_log_level_t *)field));
        break;
    case K_LOGFORMAT:
        fprintf(out, "%s",
                (*(const dx_log_format_t *)field == DX_LOG_FMT_COMBINED) ? "combined" : "common");
        break;
    case K_ADDHEADER:
        break;
    default:
        break;
    }
}

void dx_config_print(const dx_settings_t *s, FILE *out)
{
    size_t i;

    if (s == NULL || out == NULL) {
        return;
    }
    for (i = 0; i < dx_key_count; i++) {
        const dx_key_t *key = &dx_keys[i];

        if (key->kind == K_ADDHEADER) {
            size_t j;

            for (j = 0; j < s->add_header_count; j++) {
                fprintf(out, "%s = %s\n", key->name, s->add_header[j]);
            }
            continue;
        }
        fprintf(out, "%s = ", key->name);
        print_value(s, key, out);
        fputc('\n', out);
    }
}
