/*
 * main.c - Entry point: configuration merge, command line parsing, start-up.
 *
 * Sources, highest priority first: command line flags (including -o), then
 * DANTRIX_<KEY> environment variables, then the configuration file, then the
 * built-in defaults. Every value goes through the same key registry in
 * config.c, so a flag, a file line and an override cannot drift apart.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "dantrix.h"
#include "file.h"
#include "log.h"
#include "net.h"
#include "server.h"
#include "util.h"

#define DX_EXIT_OK 0
#define DX_EXIT_FATAL 1
#define DX_EXIT_USAGE 2

/*
 * Reload context: what is needed to rebuild the settings from scratch on
 * SIGHUP. The command line has the highest priority, so every flag is recorded
 * and replayed in order after the file and the environment. The list grows once
 * at start-up and is read-only afterwards.
 *
 * The key is copied into a small fixed array (keys are short); the value is
 * borrowed from argv, which lives for the whole process and may be a long path
 * that must not be truncated into a scratch buffer.
 */
#define DX_MAX_CLI_ASSIGN 128
typedef struct {
    const char *config_path; /* may be NULL */
    struct {
        char key[DX_CFG_STR_MAX];
        const char *value;
    } cli[DX_MAX_CLI_ASSIGN];
    size_t cli_count;
} dx_reload_ctx_t;

/*
 * Stable storage for a reloaded Cache-Control value. Two buffers are used and
 * flipped on every reload: workers copy the pointer (under the reload mutex)
 * and then read the STRING on the hot path without any lock, so a single
 * shared buffer could be rewritten mid-response and emit a mixed header.
 */
static char g_cache_control_buf[2][DX_CFG_STR_MAX];
static unsigned g_cache_control_slot;

/* The context the command line flags record into, set up in main(). */
static dx_reload_ctx_t *g_reload_ctx;

static void reload_record(dx_reload_ctx_t *ctx, const char *key, const char *value)
{
    if (ctx == NULL || key == NULL || value == NULL || ctx->cli_count >= DX_MAX_CLI_ASSIGN) {
        return;
    }
    if (dx_strlcpy(ctx->cli[ctx->cli_count].key, key, sizeof(ctx->cli[0].key)) >=
        sizeof(ctx->cli[0].key)) {
        return; /* key too long to be a real key: skip recording */
    }
    /* The value is borrowed: it may be a 4096-byte path and argv outlives the
     * reload, so no truncating copy is made. */
    ctx->cli[ctx->cli_count].value = value;
    ctx->cli_count++;
}

/*
 * Apply one flag value through the key registry (which validates the range)
 * and record it for the SIGHUP replay. No intermediate "key=value" string is
 * built, so a long path value cannot be truncated.
 */
static int assign(dx_settings_t *s, const char *src, const char *key, const char *value, FILE *msg)
{
    reload_record(g_reload_ctx, key, value);
    return dx_config_set(s, key, value, src, msg);
}

/*
 * Rebuild the settings on SIGHUP: defaults, then the configuration file, then
 * the environment, then the recorded command line assignments (highest
 * priority). On any error the previous settings are kept and -1 is returned.
 * The reloadable subset is written into reload; the rest is applied to new
 * connections where it makes sense.
 */
static int reload_cb(const void *vctx, dx_worker_reload_t *reload)
{
    const dx_reload_ctx_t *ctx = (const dx_reload_ctx_t *)vctx;
    dx_settings_t fresh;
    size_t i;
    FILE *msg = stderr;

    dx_config_defaults(&fresh);
    if (ctx->config_path != NULL && dx_config_file(&fresh, ctx->config_path, msg) != 0) {
        return -1;
    }
    if (dx_config_env(&fresh, msg) != 0) {
        return -1;
    }
    for (i = 0; i < ctx->cli_count; i++) {
        if (dx_config_set(&fresh, ctx->cli[i].key, ctx->cli[i].value, "cli", msg) != 0) {
            return -1;
        }
    }
    if (dx_config_validate(&fresh, msg) != 0) {
        return -1;
    }

    /* Reloadable subset, per the README key table. */
    pthread_mutex_lock(&reload->mu);
    reload->static_opts.sendfile = fresh.sendfile;
    reload->static_opts.sendfile_chunk = fresh.sendfile_chunk;
    reload->static_opts.etag = fresh.etag;
    reload->static_opts.server_tokens = fresh.server_tokens;
    reload->static_opts.hidden_files = fresh.hidden_files;
    unsigned slot = g_cache_control_slot ^ 1u;

    dx_strlcpy(g_cache_control_buf[slot], fresh.cache_control, sizeof(g_cache_control_buf[slot]));
    g_cache_control_slot = slot;
    reload->static_opts.cache_control = g_cache_control_buf[slot];

    reload->life.keepalive_requests = fresh.keepalive_requests;
    reload->life.header_timeout_ms = fresh.header_timeout_ms;
    reload->life.body_timeout_ms = fresh.body_timeout_ms;
    reload->life.send_timeout_ms = fresh.send_timeout_ms;
    reload->life.keepalive_timeout_ms = fresh.keepalive_timeout_ms;
    reload->life.shutdown_timeout_ms = fresh.shutdown_timeout_ms;
    reload->life.max_body_size = fresh.max_body_size;

    reload->limits.max_request_line = fresh.max_request_line;
    reload->limits.max_header_size = fresh.max_header_size;
    reload->limits.max_headers = fresh.max_headers;
    pthread_mutex_unlock(&reload->mu);
    return 0;
}

static void print_usage(FILE *out)
{
    const char *root = DX_DEFAULT_ROOT;
    const char *index_name = DX_DEFAULT_INDEX;
    const char *default_type = DX_DEFAULT_MIME;

    fprintf(out,
            "Dantrix %s - epoll based HTTP/1.1 server\n"
            "\n"
            "Usage:\n"
            "  dantrix [OPTION]...\n"
            "\n"
            "General:\n"
            "  -c, --config FILE        Configuration file (INI style)\n"
            "  -t, --test-config        Validate the configuration and exit\n"
            "      --print-config       Print the effective configuration and exit\n"
            "  -o, --option KEY=VALUE   Override any configuration key (repeatable)\n"
            "  -h, --help               Show this help\n"
            "  -V, --version            Show version and compiled features\n"
            "\n"
            "Network:\n"
            "  -b, --bind ADDR          Bind address                       [%s]\n"
            "  -p, --port PORT          TCP port                           [%d]\n"
            "      --backlog N          listen() queue                     [%d]\n"
            "      --max-conns N        Connection limit per worker        [%d]\n"
            "  -w, --workers N          Worker threads (0 = one per CPU)   [1]\n"
            "      --no-reuseport       Shared listener with EPOLLEXCLUSIVE\n"
            "      --cpu-affinity       Pin worker i to CPU i\n"
            "\n"
            "Static files:\n"
            "  -r, --root DIR           Document root                       [%s]\n"
            "      --index NAME         Directory index file               [%s]\n"
            "      --directory-listing  on|off (listing when no index)     [off]\n"
            "      --sendfile           on|off (else pread+send)           [on]\n"
            "      --sendfile-chunk N   Bytes per sendfile call            [1m]\n"
            "      --etag               on|off (send and evaluate ETag)    [on]\n"
            "      --follow-symlinks    on|off (links must stay in root)    [off]\n"
            "      --hidden-files       on|off (serve dot segments)        [off]\n"
            "      --mime-types FILE    Extra mime.types table             [none]\n"
            "      --default-type TYPE  Type for unknown extensions        [%s]\n"
            "      --cache-control VAL  Cache-Control header value         [none]\n"
            "      --server-tokens      on|off (Server: Dantrix/ver)       [on]\n"
            "\n"
            "HTTP limits:\n"
            "      --max-request-line N Request line limit in bytes        [%d]\n"
            "      --max-header-size N  Whole-request limit in bytes        [%d]\n"
            "      --max-headers N      Maximum header count per request   [%d]\n"
            "      --max-body-size N    Request body read and discarded     [1m]\n"
            "\n"
            "Connections and timeouts:\n"
            "      --keepalive-requests N  Requests before closing       [1000]\n"
            "      --header-timeout T   Request header limit (s/ms/m)   [10s]\n"
            "      --body-timeout T     Request body limit              [30s]\n"
            "      --send-timeout T     Response write limit            [30s]\n"
            "      --keepalive-timeout T Idle connection limit          [30s]\n"
            "      --shutdown-timeout T Graceful shutdown budget        [10s]\n"
            "\n"
            "Logging:\n"
            "      --error-log DEST     stderr, stdout, or a file path     [stderr]\n"
            "      --log-level LEVEL    debug, info, warn, error           [info]\n"
            "      --access-log DEST    off, stderr, stdout, or a file     [off]\n"
            "      --log-format FMT     common or combined                 [combined]\n"
            "\n"
            "Process:\n"
            "      --daemon             Run in the background\n"
            "      --pid-file FILE      Write the PID to this file\n"
            "      --user NAME          Drop privileges to this user after bind\n"
            "      --group NAME         Drop privileges to this group after bind\n"
            "      --status-path PATH   JSON status endpoint (loopback only) [off]\n"
            "\n"
            "Exit codes: 0 ok, 1 fatal error, 2 invalid arguments.\n",
            DX_VERSION_STRING, DX_DEFAULT_BIND, DX_DEFAULT_PORT, DX_DEFAULT_BACKLOG,
            DX_DEFAULT_MAX_CONNS, root, index_name, default_type, 8 * 1024, 16 * 1024, 100);
}

static void print_version(FILE *out)
{
    fprintf(out, "%s %s\n", DX_PROGRAM_NAME, DX_VERSION_STRING);
}

int main(int argc, char **argv)
{
    dx_settings_t set;
    dx_server_config_t cfg;
    dx_fs_t *fs = NULL;
    dx_mime_t *mime = NULL;
    FILE *msg = stderr;
    const char *config_path = NULL;
    dx_reload_ctx_t reload_ctx;
    int workers = 1;
    int rc = DX_EXIT_OK;
    bool test_config = false;
    bool print_config = false;
    int opt;

    static const struct option long_opts[] = {{"help", no_argument, NULL, 'h'},
                                              {"version", no_argument, NULL, 'V'},
                                              {"config", required_argument, NULL, 'c'},
                                              {"test-config", no_argument, NULL, 't'},
                                              {"print-config", no_argument, NULL, 'q'},
                                              {"option", required_argument, NULL, 'o'},
                                              {"bind", required_argument, NULL, 'b'},
                                              {"port", required_argument, NULL, 'p'},
                                              {"backlog", required_argument, NULL, 'B'},
                                              {"max-conns", required_argument, NULL, 'M'},
                                              {"root", required_argument, NULL, 'r'},
                                              {"index", required_argument, NULL, 'X'},
                                              {"directory-listing", required_argument, NULL, 'D'},
                                              {"sendfile", required_argument, NULL, 'S'},
                                              {"sendfile-chunk", required_argument, NULL, 'K'},
                                              {"etag", required_argument, NULL, 'G'},
                                              {"follow-symlinks", required_argument, NULL, 'Y'},
                                              {"hidden-files", required_argument, NULL, 'U'},
                                              {"mime-types", required_argument, NULL, 'T'},
                                              {"default-type", required_argument, NULL, 'O'},
                                              {"cache-control", required_argument, NULL, 'C'},
                                              {"server-tokens", required_argument, NULL, 'Q'},
                                              {"keepalive-requests", required_argument, NULL, 'k'},
                                              {"header-timeout", required_argument, NULL, 1000},
                                              {"body-timeout", required_argument, NULL, 1001},
                                              {"send-timeout", required_argument, NULL, 1002},
                                              {"keepalive-timeout", required_argument, NULL, 1003},
                                              {"shutdown-timeout", required_argument, NULL, 1004},
                                              {"max-body-size", required_argument, NULL, 'Z'},
                                              {"max-request-line", required_argument, NULL, 'R'},
                                              {"max-header-size", required_argument, NULL, 'H'},
                                              {"max-headers", required_argument, NULL, 'N'},
                                              {"workers", required_argument, NULL, 'w'},
                                              {"no-reuseport", no_argument, NULL, 'P'},
                                              {"cpu-affinity", no_argument, NULL, 'J'},
                                              {"access-log", required_argument, NULL, 'A'},
                                              {"log-format", required_argument, NULL, 'F'},
                                              {"error-log", required_argument, NULL, 'E'},
                                              {"log-level", required_argument, NULL, 'L'},
                                              {"daemon", no_argument, NULL, 1010},
                                              {"pid-file", required_argument, NULL, 1011},
                                              {"user", required_argument, NULL, 1012},
                                              {"group", required_argument, NULL, 1013},
                                              {"status-path", required_argument, NULL, 1014},
                                              {NULL, 0, NULL, 0}};

    /* Pass 1: find the configuration file so it is loaded before the flags. */
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hVc:o:b:p:w:r:t", long_opts, NULL)) != -1) {
        if (opt == 'c') {
            config_path = optarg;
        }
    }

    dx_config_defaults(&set);
    memset(&reload_ctx, 0, sizeof(reload_ctx));
    reload_ctx.config_path = config_path;
    g_reload_ctx = &reload_ctx;

    /* Lowest tiers first: the configuration file, then the environment. */
    if (config_path != NULL) {
        if (dx_config_file(&set, config_path, msg) != 0) {
            return DX_EXIT_USAGE;
        }
    }
    if (dx_config_env(&set, msg) != 0) {
        return DX_EXIT_USAGE;
    }

    /*
     * Pass 2: apply every command line flag in argv order, so the last one
     * wins exactly as the documented priority requires.
     */
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hVb:p:w:r:PJo:c:t", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(stdout);
            return DX_EXIT_OK;
        case 'V':
            print_version(stdout);
            return DX_EXIT_OK;
        case 'c':
            /* Already consumed in pass 1. */
            break;
        case 't':
            test_config = true;
            break;
        case 'q':
            print_config = true;
            break;
        case 'o':
            /*
             * -o is the highest-priority source, so it has to be recorded for
             * the SIGHUP replay exactly like the other flags. Split it into key
             * and value; the pair is applied directly (no intermediate
             * "key=value" string), so a long path value is never truncated.
             */
            {
                const char *eq = strchr(optarg, '=');

                if (eq == NULL || eq == optarg) {
                    fprintf(stderr, "dantrix: -o: expected key=value, got '%s'\n", optarg);
                    return DX_EXIT_USAGE;
                }
                {
                    /* reload_record() copies the key, so this scratch buffer
                     * only has to live for the call. */
                    char key[DX_CFG_STR_MAX];

                    if ((size_t)(eq - optarg) >= sizeof(key)) {
                        fprintf(stderr, "dantrix: -o: key too long\n");
                        return DX_EXIT_USAGE;
                    }
                    memcpy(key, optarg, (size_t)(eq - optarg));
                    key[eq - optarg] = '\0';
                    reload_record(g_reload_ctx, key, eq + 1);
                    if (dx_config_set(&set, key, eq + 1, "-o", msg) != 0) {
                        return DX_EXIT_USAGE;
                    }
                }
            }
            break;
        case 'b':
            if (assign(&set, "--bind", "bind", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'p':
            if (assign(&set, "--port", "port", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'B':
            if (assign(&set, "--backlog", "backlog", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'M':
            if (assign(&set, "--max-conns", "max_connections", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'R':
            if (assign(&set, "--max-request-line", "max_request_line", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'H':
            if (assign(&set, "--max-header-size", "max_header_size", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'N':
            if (assign(&set, "--max-headers", "max_headers", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'w':
            if (assign(&set, "--workers", "workers", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'P':
            set.reuseport = false;
            break;
        case 'J':
            set.cpu_affinity = true;
            break;
        case 'r':
            if (assign(&set, "--root", "root", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'X':
            if (assign(&set, "--index", "index", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'D':
            if (assign(&set, "--directory-listing", "directory_listing", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'S':
            if (assign(&set, "--sendfile", "sendfile", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'K':
            if (assign(&set, "--sendfile-chunk", "sendfile_chunk", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'G':
            if (assign(&set, "--etag", "etag", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'Y':
            if (assign(&set, "--follow-symlinks", "follow_symlinks", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'U':
            if (assign(&set, "--hidden-files", "hidden_files", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'T':
            if (assign(&set, "--mime-types", "mime_types", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'O':
            if (assign(&set, "--default-type", "default_type", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'C':
            if (assign(&set, "--cache-control", "cache_control", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'Q':
            if (assign(&set, "--server-tokens", "server_tokens", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'k':
            if (assign(&set, "--keepalive-requests", "keepalive_requests", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1000:
            if (assign(&set, "--header-timeout", "header_timeout", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1001:
            if (assign(&set, "--body-timeout", "body_timeout", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1002:
            if (assign(&set, "--send-timeout", "send_timeout", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1003:
            if (assign(&set, "--keepalive-timeout", "keepalive_timeout", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1004:
            if (assign(&set, "--shutdown-timeout", "shutdown_timeout", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'Z':
            if (assign(&set, "--max-body-size", "max_body_size", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'A':
            if (strcmp(optarg, "off") == 0) {
                dx_strlcpy(set.access_log, "off", sizeof(set.access_log));
            } else if (assign(&set, "--access-log", "access_log", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'F':
            if (assign(&set, "--log-format", "log_format", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'E':
            if (assign(&set, "--error-log", "error_log", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 'L':
            if (assign(&set, "--log-level", "log_level", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1010:
            set.daemonize = true;
            break;
        case 1011:
            if (assign(&set, "--pid-file", "pid_file", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1012:
            if (assign(&set, "--user", "user", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1013:
            if (assign(&set, "--group", "group", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        case 1014:
            if (assign(&set, "--status-path", "status_path", optarg, msg) != 0) {
                return DX_EXIT_USAGE;
            }
            break;
        default:
            print_usage(stderr);
            return DX_EXIT_USAGE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "dantrix: unexpected argument '%s'\n", argv[optind]);
        print_usage(stderr);
        return DX_EXIT_USAGE;
    }

    /* Deferred diagnostics: they must see the whole merged configuration. */
    if (print_config) {
        if (dx_config_validate(&set, msg) != 0) {
            return DX_EXIT_USAGE;
        }
        dx_config_print(&set, stdout);
        return DX_EXIT_OK;
    }
    if (test_config) {
        if (dx_config_validate(&set, msg) != 0 || dx_config_check_root(&set, msg) != 0) {
            return DX_EXIT_USAGE;
        }
        return DX_EXIT_OK;
    }

    /*
     * Cross-key validation and the document root check happen before the
     * server opens any log file, so a bad configuration never creates one.
     */
    if (dx_config_validate(&set, msg) != 0 || dx_config_check_root(&set, msg) != 0) {
        return DX_EXIT_USAGE;
    }

    /* Features that are documented but not implemented yet stay explicit. */
    if (set.edge_triggered) {
        fprintf(stderr, "dantrix: edge_triggered is not implemented yet (planned for v0.7)\n");
    }
    if (set.add_header_count > 0) {
        fprintf(stderr, "dantrix: add_header is not implemented yet (%zu header(s) ignored)\n",
                set.add_header_count);
    }
    if (set.tcp_defer_accept_ms != 0 || set.tcp_fastopen != 0 || set.socket_rcvbuf != 0 ||
        set.socket_sndbuf != 0) {
        fprintf(stderr,
                "dantrix: tcp_defer_accept/tcp_fastopen/socket_rcvbuf/socket_sndbuf are not "
                "implemented yet\n");
    }

    /*
     * A relative pid_file would be written under / after the daemon
     * chdir("/")s, so it is made absolute here while the cwd is still the
     * one the operator ran from.
     */
    if (set.pid_file[0] != '\0' && set.pid_file[0] != '/') {
        char cwd[DX_CFG_PATH_MAX];
        char abs[DX_CFG_PATH_MAX];

        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            if (snprintf(abs, sizeof(abs), "%s/%s", cwd, set.pid_file) < (int)sizeof(abs)) {
                dx_strlcpy(set.pid_file, abs, sizeof(set.pid_file));
            }
        }
    }

    /*
     * The document root is opened before the listener so that a bad root is a
     * start-up error rather than a stream of 404s.
     */
    fs = dx_fs_create(set.root, set.index, set.directory_listing, set.follow_symlinks);
    if (fs == NULL) {
        fprintf(stderr, "dantrix: cannot use document root '%s': %s\n", set.root,
                dx_strerror(errno));
        return DX_EXIT_FATAL;
    }
    /*
     * Report which path resolution the kernel provides. The probe used to
     * exist but was never called, so the operator had no way to tell whether
     * RESOLVE_BENEATH or the manual walk was enforcing the root.
     */
    if (!dx_fs_has_openat2()) {
        fprintf(stderr, "dantrix: path resolution: manual walk (openat2 unavailable)\n");
    } else if (dx_fs_openat2_blocked()) {
        /*
         * The probe succeeded but a real openat2 fails with EPERM: a seccomp
         * policy. Without the warning every request would 403 with no hint
         * why, because the manual walk is never tried in this case.
         */
        fprintf(stderr, "dantrix: WARNING: openat2() is blocked by policy (EPERM); "
                        "every request will be refused with 403\n");
    } else {
        fprintf(stderr, "dantrix: path resolution: openat2(RESOLVE_BENEATH)\n");
    }
    mime = dx_mime_create(set.mime_types, set.default_type);
    if (mime == NULL) {
        fprintf(stderr, "dantrix: cannot build the MIME table\n");
        dx_fs_destroy(fs);
        return DX_EXIT_FATAL;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = set.bind;
    cfg.port = set.port;
    cfg.backlog = set.backlog;
    cfg.max_conns = set.max_connections;
    /*
     * workers == 0 means "one worker per online CPU"; resolve it here so the
     * server layer always receives a concrete count.
     */
    workers = set.workers;
    if (workers == 0) {
        workers = (int)dx_cpu_count();
    }
    /*
     * The registry caps "workers" at 1024, but "auto" bypasses it because it
     * is resolved here. Clamp with an explanation instead of letting
     * server_run_multi() fail with a bare EINVAL on a 2048-core machine.
     */
    if (workers > 1024) {
        fprintf(stderr, "dantrix: --workers %d exceeds the maximum of 1024\n", workers);
        return DX_EXIT_USAGE;
    }
    cfg.workers = workers;
    cfg.reuseport = set.reuseport;
    cfg.cpu_affinity = set.cpu_affinity;
    cfg.limits.max_request_line = set.max_request_line;
    cfg.limits.max_header_size = set.max_header_size;
    cfg.limits.max_headers = set.max_headers;
    cfg.log_format = set.log_format;
    cfg.log_level = set.log_level;
    cfg.error_log_path = set.error_log;
    cfg.access_log_path = set.access_log;
    cfg.daemonize = set.daemonize;
    cfg.pid_file = set.pid_file;
    cfg.user = set.user;
    cfg.group = set.group;
    cfg.status_path = set.status_path;
    cfg.reload_cb = reload_cb;
    cfg.reload_ctx = &reload_ctx;
    cfg.fs = fs;
    cfg.mime = mime;
    cfg.static_opts.sendfile = set.sendfile;
    cfg.static_opts.sendfile_chunk = set.sendfile_chunk;
    cfg.static_opts.etag = set.etag;
    cfg.static_opts.server_tokens = set.server_tokens;
    cfg.static_opts.hidden_files = set.hidden_files;
    cfg.static_opts.cache_control = set.cache_control;
    cfg.life.keepalive_requests = set.keepalive_requests;
    cfg.life.header_timeout_ms = set.header_timeout_ms;
    cfg.life.body_timeout_ms = set.body_timeout_ms;
    cfg.life.send_timeout_ms = set.send_timeout_ms;
    cfg.life.keepalive_timeout_ms = set.keepalive_timeout_ms;
    cfg.life.shutdown_timeout_ms = set.shutdown_timeout_ms;
    cfg.life.max_body_size = set.max_body_size;

    if (dx_server_run(&cfg) != 0) {
        rc = DX_EXIT_FATAL;
    }

    dx_mime_destroy(mime);
    dx_fs_destroy(fs);
    return rc;
}
