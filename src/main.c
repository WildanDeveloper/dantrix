/*
 * main.c - Entry point: command line parsing and startup.
 *
 * v0.1 supports the flags that make sense before the HTTP layer exists
 * (network, limits, logging). Flags that belong to later milestones are
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
#include "log.h"
#include "net.h"
#include "server.h"
#include "util.h"

#define DX_EXIT_OK 0
#define DX_EXIT_FATAL 1
#define DX_EXIT_USAGE 2

static void print_usage(FILE *out)
{
    fprintf(out,
            "Dantrix %s - epoll based HTTP/1.1 server (v0.1: TCP echo core)\n"
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
            "  -w, --workers N          Worker count (only 1 in v0.1)      [1]\n"
            "\n"
            "Logging:\n"
            "      --error-log DEST     stderr, stdout, or a file path     [stderr]\n"
            "      --log-level LEVEL    debug, info, warn, error           [info]\n"
            "\n"
            "Accepted for compatibility, not implemented yet:\n"
            "  -r, --root DIR           Document root (used from v0.3)\n"
            "      --access-log DEST    Access log (used from v0.2)\n"
            "\n"
            "Exit codes: 0 ok, 1 fatal error, 2 invalid arguments.\n",
            DX_VERSION_STRING, DX_DEFAULT_BIND, DX_DEFAULT_PORT, DX_DEFAULT_BACKLOG,
            DX_DEFAULT_MAX_CONNS);
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
    int port = DX_DEFAULT_PORT;
    int backlog = DX_DEFAULT_BACKLOG;
    int max_conns_arg = DX_DEFAULT_MAX_CONNS;
    int workers = 1;
    int rc = DX_EXIT_OK;
    dx_log_t log;
    dx_log_level_t level = DX_LOG_INFO;
    dx_server_config_t cfg;
    FILE *log_file = NULL;
    int opt;

    static const struct option long_opts[] = {{"help", no_argument, NULL, 'h'},
                                              {"version", no_argument, NULL, 'V'},
                                              {"bind", required_argument, NULL, 'b'},
                                              {"port", required_argument, NULL, 'p'},
                                              {"backlog", required_argument, NULL, 'B'},
                                              {"max-conns", required_argument, NULL, 'M'},
                                              {"workers", required_argument, NULL, 'w'},
                                              {"root", required_argument, NULL, 'r'},
                                              {"access-log", required_argument, NULL, 'A'},
                                              {"error-log", required_argument, NULL, 'E'},
                                              {"log-level", required_argument, NULL, 'L'},
                                              {NULL, 0, NULL, 0}};

    while ((opt = getopt_long(argc, argv, "hVb:p:w:r:", long_opts, NULL)) != -1) {
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
        case 'w':
            if (!dx_parse_int(optarg, 1, 1024, &workers)) {
                fprintf(stderr, "dantrix: invalid --workers '%s'\n", optarg);
                return DX_EXIT_USAGE;
            }
            if (workers != 1) {
                fprintf(stderr, "dantrix: only one worker is supported in v0.1 "
                                "(multi-worker lands in v0.5)\n");
                return DX_EXIT_USAGE;
            }
            break;
        case 'r':
            fprintf(stderr, "dantrix: warning: --root is accepted but static file "
                            "serving starts in v0.3\n");
            break;
        case 'A':
            if (strcmp(optarg, "off") != 0) {
                fprintf(stderr, "dantrix: warning: --access-log is accepted but the "
                                "access log starts in v0.2\n");
            }
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

    /*
     * The destination is opened after argument validation, so that an invalid
     * flag never creates an empty log file.
     */
    log_file = open_log_dest(log_dest);
    if (log_file == NULL) {
        fprintf(stderr, "dantrix: cannot open error log '%s': %s\n", log_dest, dx_strerror(errno));
        return DX_EXIT_FATAL;
    }

    log.out = log_file;
    log.level = level;

    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = bind_addr;
    cfg.port = port;
    cfg.backlog = backlog;
    cfg.max_conns = (size_t)max_conns_arg;
    cfg.log = &log;

    if (dx_server_run(&cfg) != 0) {
        rc = DX_EXIT_FATAL;
    }

    /*
     * log_file is always open here: a failed open exits with DX_EXIT_FATAL
     * above, and stderr/stdout never need closing by hand.
     */
    (void)fclose(log_file);
    return rc;
}
