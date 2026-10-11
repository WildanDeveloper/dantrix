/*
 * main.c - Entry point: command line parsing and startup.
 *
 * v0.2 adds the HTTP core: strict request parsing with configurable header
 * limits and the access log. Flags that belong to later milestones are still
 * accepted with an explicit warning so scripts written against the final CLI
 * do not break, but their behaviour is documented as not implemented yet.
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dantrix.h"
#include "file.h"
#include "http_parser.h"
#include "log.h"
#include "net.h"
#include "server.h"
#include "util.h"

#define DX_EXIT_OK 0
#define DX_EXIT_FATAL 1
#define DX_EXIT_USAGE 2

/* Parse an on/off flag. Returns 1 when parsed, 0 when the word is unknown. */
static int parse_on_off(const char *value, bool *out)
{
    if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
        *out = true;
        return 1;
    }
    if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
        *out = false;
        return 1;
    }
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
            "  -h, --help               Show this help\n"
            "  -V, --version            Show version and compiled features\n"
            "\n"
            "Network:\n"
            "  -b, --bind ADDR          Bind address                       [%s]\n"
            "  -p, --port PORT          TCP port                           [%d]\n"
            "      --backlog N          listen() queue                     [%d]\n"
            "      --max-conns N        Connection limit                   [%d]\n"
            "  -w, --workers N          Worker count (only 1 until v0.5)   [1]\n"
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
            "      --max-header-size N  Whole-request limit in bytes (k/m/g"
            " suffixes allowed)      [%d]\n"
            "      --max-headers N      Maximum header count per request   [%d]\n"
            "\n"
            "Logging:\n"
            "      --error-log DEST     stderr, stdout, or a file path     [stderr]\n"
            "      --log-level LEVEL    debug, info, warn, error           [info]\n"
            "      --access-log DEST    off, stderr, stdout, or a file     [%s]\n"
            "      --log-format FMT     common or combined                 [common]\n"
            "\n"
            "Exit codes: 0 ok, 1 fatal error, 2 invalid arguments.\n",
            DX_VERSION_STRING, DX_DEFAULT_BIND, DX_DEFAULT_PORT, DX_DEFAULT_BACKLOG,
            DX_DEFAULT_MAX_CONNS, root, index_name, default_type, 8 * 1024, 16 * 1024, 100, "off");
}

static void print_version(FILE *out)
{
    fprintf(out, "%s %s\n", DX_PROGRAM_NAME, DX_VERSION_STRING);
}

/* Open a log destination: "stdout", "stderr" or a file path. */
static FILE *open_log_dest(const char *dest)
{
    FILE *f;

    if (dest == NULL || strcmp(dest, "stderr") == 0) {
        return stderr;
    }
    if (strcmp(dest, "stdout") == 0) {
        return stdout;
    }

    f = fopen(dest, "a");
    if (f != NULL) {
        setvbuf(f, NULL, _IOLBF, 0);
    }
    return f;
}

int main(int argc, char **argv)
{
    const char *bind_addr = DX_DEFAULT_BIND;
    const char *log_dest = "stderr";
    const char *log_level_name = "info";
    const char *access_dest = NULL; /* NULL = off */
    const char *log_format_name = "common";
    const char *root = DX_DEFAULT_ROOT;
    const char *index_name = DX_DEFAULT_INDEX;
    const char *mime_types = NULL;
    const char *default_type = DX_DEFAULT_MIME;
    const char *cache_control = "";
    bool directory_listing = false;
    bool sendfile = true;
    bool etag = true;
    bool follow_symlinks = false;
    bool hidden_files = false;
    bool server_tokens = true;
    size_t sendfile_chunk = 1024u * 1024u;
    int port = DX_DEFAULT_PORT;
    int backlog = DX_DEFAULT_BACKLOG;
    int max_conns_arg = DX_DEFAULT_MAX_CONNS;
    int max_headers_arg = 100;
    size_t max_request_line = 8u * 1024u;
    size_t max_header_size = 16u * 1024u;
    int workers = 1;
    int rc = DX_EXIT_OK;
    dx_log_t log;
    dx_log_t access_logger;
    dx_log_level_t level = DX_LOG_INFO;
    dx_log_format_t log_format = DX_LOG_FMT_COMMON;
    dx_http_limits_t limits;
    dx_server_config_t cfg;
    dx_fs_t *fs = NULL;
    dx_mime_t *mime = NULL;
    FILE *log_file = NULL;
    FILE *access_file = NULL;
    int opt;

    static const struct option long_opts[] = {{"help", no_argument, NULL, 'h'},
                                              {"version", no_argument, NULL, 'V'},
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
                                              {"max-request-line", required_argument, NULL, 'R'},
                                              {"max-header-size", required_argument, NULL, 'H'},
                                              {"max-headers", required_argument, NULL, 'N'},
                                              {"workers", required_argument, NULL, 'w'},
                                              {"root", required_argument, NULL, 'r'},
                                              {"access-log", required_argument, NULL, 'A'},
                                              {"log-format", required_argument, NULL, 'F'},
                                              {"error-log", required_argument, NULL, 'E'},
                                              {"log-level", required_argument, NULL, 'L'},
                                              {NULL, 0, NULL, 0}};

    while ((opt = getopt_long(argc, argv, "hVb:p:wr:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(stdout);
            return DX_EXIT_OK;
        case 'V':
            print_version(stdout);
            return DX_EXIT_OK;
        case 'b':
            bind_addr = optarg;
            break;
        case 'p':
            if (!dx_parse_int(optarg, 1, 65535, &port)) {
                fprintf(stderr, "dantrix: invalid port '%s' (expected 1..65535)\n", optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'B':
            if (!dx_parse_int(optarg, 1, 65535, &backlog)) {
                fprintf(stderr, "dantrix: invalid backlog '%s'\n", optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'M':
            if (!dx_parse_int(optarg, 1, 1000000, &max_conns_arg)) {
                fprintf(stderr, "dantrix: invalid --max-conns '%s'\n", optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'R':
            if (!dx_parse_size(optarg, &max_request_line) || max_request_line < 64u ||
                max_request_line > 1024u * 1024u) {
                fprintf(stderr, "dantrix: invalid --max-request-line '%s' (expected 64..1m)\n",
                        optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'H':
            if (!dx_parse_size(optarg, &max_header_size) || max_header_size < 256u ||
                max_header_size > 16u * 1024u * 1024u) {
                fprintf(stderr, "dantrix: invalid --max-header-size '%s' (expected 256..16m)\n",
                        optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'N':
            if (!dx_parse_int(optarg, 1, (int)DX_MAX_HEADERS_LIMIT, &max_headers_arg)) {
                fprintf(stderr, "dantrix: invalid --max-headers '%s' (expected 1..%d)\n", optarg,
                        (int)DX_MAX_HEADERS_LIMIT);
                return DX_EXIT_USAGE;
            }
            break;
        case 'w':
            if (!dx_parse_int(optarg, 1, 1024, &workers)) {
                fprintf(stderr, "dantrix: invalid --workers '%s'\n", optarg);
                return DX_EXIT_USAGE;
            }
            if (workers != 1) {
                fprintf(stderr, "dantrix: only one worker is supported until v0.5 "
                                "(multi-worker lands in v0.5)\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'r':
            root = optarg;
            break;
        case 'X':
            index_name = optarg;
            break;
        case 'D':
            if (!parse_on_off(optarg, &directory_listing)) {
                fprintf(stderr, "dantrix: --directory-listing expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'S':
            if (!parse_on_off(optarg, &sendfile)) {
                fprintf(stderr, "dantrix: --sendfile expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'K':
            if (!dx_parse_size(optarg, &sendfile_chunk) || sendfile_chunk < 4096u ||
                sendfile_chunk > 64u * 1024u * 1024u) {
                fprintf(stderr, "dantrix: invalid --sendfile-chunk '%s' (expected 4k..64m)\n",
                        optarg);
                return DX_EXIT_USAGE;
            }
            break;
        case 'G':
            if (!parse_on_off(optarg, &etag)) {
                fprintf(stderr, "dantrix: --etag expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'Y':
            if (!parse_on_off(optarg, &follow_symlinks)) {
                fprintf(stderr, "dantrix: --follow-symlinks expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'U':
            if (!parse_on_off(optarg, &hidden_files)) {
                fprintf(stderr, "dantrix: --hidden-files expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'T':
            mime_types = optarg;
            break;
        case 'O':
            default_type = optarg;
            break;
        case 'C':
            cache_control = optarg;
            break;
        case 'Q':
            if (!parse_on_off(optarg, &server_tokens)) {
                fprintf(stderr, "dantrix: --server-tokens expects on or off\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'A':
            if (strcmp(optarg, "off") == 0) {
                access_dest = NULL;
            } else {
                access_dest = optarg;
            }
            break;
        case 'F':
            log_format_name = optarg;
            break;
        case 'E':
            log_dest = optarg;
            break;
        case 'L':
            log_level_name = optarg;
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

    if (!dx_log_level_parse(log_level_name, &level)) {
        fprintf(stderr, "dantrix: unknown log level '%s'\n", log_level_name);
        return DX_EXIT_USAGE;
    }
    if (strcmp(log_format_name, "common") == 0) {
        log_format = DX_LOG_FMT_COMMON;
    } else if (strcmp(log_format_name, "combined") == 0) {
        log_format = DX_LOG_FMT_COMBINED;
    } else {
        fprintf(stderr, "dantrix: unknown log format '%s' (expected common or combined)\n",
                log_format_name);
        return DX_EXIT_USAGE;
    }

    /*
     * The destinations are opened after argument validation, so that an
     * invalid flag never creates an empty log file.
     */
    log_file = open_log_dest(log_dest);
    if (log_file == NULL) {
        fprintf(stderr, "dantrix: cannot open error log '%s': %s\n", log_dest, dx_strerror(errno));
        return DX_EXIT_FATAL;
    }

    log.out = log_file;
    log.level = level;

    limits.max_request_line = max_request_line;
    limits.max_header_size = max_header_size;
    limits.max_headers = (size_t)max_headers_arg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = bind_addr;
    cfg.port = port;
    cfg.backlog = backlog;
    cfg.max_conns = (size_t)max_conns_arg;
    cfg.limits = limits;
    cfg.log = &log;
    cfg.log_format = log_format;

    if (access_dest != NULL) {
        access_file = open_log_dest(access_dest);
        if (access_file == NULL) {
            fprintf(stderr, "dantrix: cannot open access log '%s': %s\n", access_dest,
                    dx_strerror(errno));
            return DX_EXIT_FATAL;
        }
        access_logger.out = access_file;
        access_logger.level = level;
        cfg.access_log = &access_logger;
    }

    /*
     * The document root is opened before the listener so that a bad root is a
     * start-up error rather than a stream of 404s.
     */
    fs = dx_fs_create(root, index_name, directory_listing, follow_symlinks);
    if (fs == NULL) {
        fprintf(stderr, "dantrix: cannot use document root '%s': %s\n", root, dx_strerror(errno));
        return DX_EXIT_FATAL;
    }
    mime = dx_mime_create(mime_types, default_type);
    if (mime == NULL) {
        fprintf(stderr, "dantrix: cannot build the MIME table\n");
        dx_fs_destroy(fs);
        return DX_EXIT_FATAL;
    }

    cfg.fs = fs;
    cfg.mime = mime;
    cfg.static_opts.sendfile = sendfile;
    cfg.static_opts.sendfile_chunk = sendfile_chunk;
    cfg.static_opts.etag = etag;
    cfg.static_opts.server_tokens = server_tokens;
    cfg.static_opts.hidden_files = hidden_files;
    cfg.static_opts.cache_control = cache_control;

    if (dx_server_run(&cfg) != 0) {
        rc = DX_EXIT_FATAL;
    }

    dx_mime_destroy(mime);
    dx_fs_destroy(fs);

    /*
     * Both streams are open here: a failed open exits with DX_EXIT_FATAL
     * above, and stderr/stdout never need closing by hand.
     */
    if (access_file != NULL && access_file != stdout && access_file != stderr) {
        (void)fclose(access_file);
    }
    (void)fclose(log_file);
    return rc;
}
