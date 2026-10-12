/*
 * config.h - The effective configuration: one struct, one key registry.
 *
 * Every setting Dantrix honours lives in dx_settings_t and is described
 * exactly once in the key table in config.c. The table drives all three input
 * sources (config file, DANTRIX_* environment variables, and the -o override)
 * as well as --print-config, so a key can never drift between the parser, the
 * documentation and the defaults.
 *
 * Priority, highest first: command line flags and -o, then DANTRIX_<KEY>,
 * then the configuration file, then the built-in defaults. Within one tier the
 * last assignment wins.
 */
#ifndef DX_CONFIG_H
#define DX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "log.h"

/* Upper bounds for the string-valued keys. */
#define DX_CFG_STR_MAX 256u
#define DX_CFG_PATH_MAX 4096u

/* add_header is the only repeatable key. */
#define DX_CFG_MAX_ADD_HEADERS 16u

typedef struct {
    /* Network */
    char bind[DX_CFG_STR_MAX];
    int port;
    int workers; /* 0 = one worker per online CPU */
    int backlog;
    bool reuseport;
    size_t max_connections;
    bool cpu_affinity;
    bool edge_triggered;

    /* Timeouts, milliseconds internally */
    uint64_t header_timeout_ms;
    uint64_t body_timeout_ms;
    uint64_t send_timeout_ms;
    uint64_t keepalive_timeout_ms;
    uint64_t shutdown_timeout_ms;

    /* HTTP limits */
    size_t keepalive_requests;
    size_t max_request_line;
    size_t max_header_size;
    size_t max_headers;
    uint64_t max_body_size;

    /* Static files */
    char root[DX_CFG_PATH_MAX];
    char index[DX_CFG_STR_MAX];
    bool directory_listing;
    bool follow_symlinks;
    bool hidden_files;
    bool sendfile;
    size_t sendfile_chunk;
    bool etag;
    char mime_types[DX_CFG_PATH_MAX];
    char default_type[DX_CFG_STR_MAX];
    char cache_control[DX_CFG_STR_MAX];
    bool server_tokens;

    /* Extra response headers ("Name: value"), repeatable */
    char add_header[DX_CFG_MAX_ADD_HEADERS][DX_CFG_STR_MAX];
    size_t add_header_count;

    /* TCP socket options */
    bool tcp_nodelay;
    uint64_t tcp_defer_accept_ms;
    int tcp_fastopen;
    size_t socket_rcvbuf;
    size_t socket_sndbuf;

    /* Logging */
    char access_log[DX_CFG_PATH_MAX];
    char error_log[DX_CFG_PATH_MAX];
    dx_log_level_t log_level;
    dx_log_format_t log_format;

    /* Process and privileges */
    bool daemonize;
    char pid_file[DX_CFG_PATH_MAX];
    char user[DX_CFG_STR_MAX];
    char group[DX_CFG_STR_MAX];

    /* JSON status endpoint; empty disables it */
    char status_path[DX_CFG_STR_MAX];
} dx_settings_t;

/* Fill every field with the documented built-in default. */
void dx_config_defaults(dx_settings_t *s);

/*
 * Parse one configuration file into s. Unknown keys, malformed values and
 * values that fail their own range check are errors (return -1) and are
 * described on msg. A repeated single-value key keeps the last value and is
 * reported as a warning on msg.
 */
int dx_config_file(dx_settings_t *s, const char *path, FILE *msg);

/*
 * Apply the DANTRIX_<KEY> environment variables (uppercase key, value as in
 * the configuration file). Unknown names are ignored so unrelated variables
 * never break the start-up; a malformed value is an error.
 */
int dx_config_env(dx_settings_t *s, FILE *msg);

/*
 * Apply one "key=value" assignment (the -o flag). Same rules as a file line.
 */
int dx_config_override(dx_settings_t *s, const char *assignment, FILE *msg);

/*
 * Same as dx_config_override(), but with a caller-supplied source label so a
 * command line flag can report itself instead of the generic "-o".
 */
int dx_config_override_src(dx_settings_t *s, const char *assignment, const char *src, FILE *msg);

/*
 * Apply a key and a value directly, without going through an intermediate
 * "key=value" string. Building that string would cap a path value at the
 * buffer size and silently truncate it (DX_CFG_PATH_MAX is 4096, far above the
 * old 512-byte scratch), so every code path that holds the key and the value
 * separately uses this one. Same rules and return contract as the overrides.
 */
int dx_config_set(dx_settings_t *s, const char *key, const char *value, const char *src, FILE *msg);

/*
 * Cross-key validation (ranges that only make sense together). Returns 0, or
 * -1 with the reason on msg. This is filesystem-free, so --print-config can
 * show an effective configuration whose root is not present yet.
 */
int dx_config_validate(const dx_settings_t *s, FILE *msg);

/*
 * Filesystem validation: the document root must exist and be a readable
 * directory. Separate from dx_config_validate() so a diagnostic run can skip
 * it. Returns 0 or -1.
 */
int dx_config_check_root(const dx_settings_t *s, FILE *msg);

/* Print the effective configuration in key = value form. */
void dx_config_print(const dx_settings_t *s, FILE *out);

/* Number of keys in the registry (including add_header). */
size_t dx_config_key_count(void);

#endif /* DX_CONFIG_H */
