/* Unit tests for src/log.c (level parsing, filtering, record format). */
#include "testing.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

/* Capture one formatted record in memory instead of a file. */
static size_t capture(char *buf, size_t cap, dx_log_t *log, dx_log_level_t level, const char *msg)
{
    FILE *mem = fmemopen(buf, cap, "w");
    size_t len;

    if (mem == NULL) {
        return 0;
    }
    log->out = mem;
    dx_log_write(log, level, "%s", msg);
    fclose(mem);
    len = strlen(buf);
    return len;
}

static void test_levels(void)
{
    dx_log_level_t v = DX_LOG_ERROR;

    dx_test_begin("log/levels");

    CHECK(strcmp(dx_log_level_name(DX_LOG_DEBUG), "debug") == 0);
    CHECK(strcmp(dx_log_level_name(DX_LOG_INFO), "info") == 0);
    CHECK(strcmp(dx_log_level_name(DX_LOG_WARN), "warn") == 0);
    CHECK(strcmp(dx_log_level_name(DX_LOG_ERROR), "error") == 0);
    /* Unknown values must not dereference garbage. */
    CHECK(dx_log_level_name((dx_log_level_t)99) != NULL);

    CHECK(dx_log_level_parse("debug", &v) && v == DX_LOG_DEBUG);
    CHECK(dx_log_level_parse("info", &v) && v == DX_LOG_INFO);
    CHECK(dx_log_level_parse("warn", &v) && v == DX_LOG_WARN);
    CHECK(dx_log_level_parse("error", &v) && v == DX_LOG_ERROR);

    CHECK(!dx_log_level_parse("INFO", &v)); /* case sensitive by design */
    CHECK(!dx_log_level_parse("", &v));
    CHECK(!dx_log_level_parse("verbose", &v));
    CHECK(!dx_log_level_parse(NULL, &v));
}

static void test_filtering_and_format(void)
{
    char buf[256];
    dx_log_t log;

    dx_test_begin("log/filter");

    log.out = NULL;
    log.level = DX_LOG_INFO;

    /* Records below the level are dropped; equal and above are kept. */
    memset(buf, 0, sizeof(buf));
    CHECK(capture(buf, sizeof(buf), &log, DX_LOG_INFO, "kept") > 0);
    CHECK(strstr(buf, "kept") != NULL);
    CHECK(strstr(buf, "[info]") != NULL);

    memset(buf, 0, sizeof(buf));
    capture(buf, sizeof(buf), &log, DX_LOG_DEBUG, "dropped");
    CHECK(strstr(buf, "dropped") == NULL);

    memset(buf, 0, sizeof(buf));
    capture(buf, sizeof(buf), &log, DX_LOG_ERROR, "urgent");
    CHECK(strstr(buf, "[error]") != NULL);
    CHECK(strstr(buf, "urgent") != NULL);

    /* The record is one line ending in a newline. */
    CHECK(buf[strlen(buf) - 1] == '\n');
    CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1);

    /* A UTC timestamp prefix is present. */
    CHECK(strchr(buf, 'T') != NULL && buf[0] == '2');

    /* NULL log or NULL out never crashes. */
    dx_log_write(NULL, DX_LOG_INFO, "x");
    log.out = NULL;
    dx_log_write(&log, DX_LOG_INFO, "x");
}

/* Capture one dx_log_access call into buf via a temp file. */
static size_t capture_access(char *buf, size_t cap, dx_log_t *log, dx_log_format_t fmt,
                             const char *peer, const char *method, const char *target,
                             const char *protocol, int status, size_t bytes, const char *referer,
                             const char *user_agent)
{
    FILE *tmp = tmpfile();
    size_t got = 0;

    if (tmp == NULL) {
        return 0;
    }
    log->out = tmp;
    dx_log_access(log, fmt, peer, method, method != NULL ? strlen(method) : 0u, target,
                  target != NULL ? strlen(target) : 0u, protocol, status, bytes, referer,
                  user_agent);
    rewind(tmp);
    got = fread(buf, 1u, cap - 1u, tmp);
    buf[got] = '\0';
    fclose(tmp);
    return got;
}

static void test_access_log(void)
{
    dx_log_t log;
    char buf[1024];

    log.level = DX_LOG_INFO;

    dx_test_begin("log/access_common");

    /* Common format: peer, identd, user, [time], request, status, bytes. */
    memset(buf, 0, sizeof(buf));
    capture_access(buf, sizeof(buf), &log, DX_LOG_FMT_COMMON, "203.0.113.7", "GET", "/x",
                   "HTTP/1.1", 200, 39, NULL, NULL);
    CHECK(strncmp(buf, "203.0.113.7 - - [", 17u) == 0);
    CHECK(strstr(buf, "] \"GET /x HTTP/1.1\" 200 39") != NULL);
    /* Common format has no referer/agent. */
    CHECK(strchr(buf, '"') == buf + strlen(buf) - 1u - 0u ||
          strstr(buf, "\" \"") == NULL);
    /* One line. */
    CHECK(buf[strlen(buf) - 1u] == '\n');

    dx_test_begin("log/access_combined");

    memset(buf, 0, sizeof(buf));
    capture_access(buf, sizeof(buf), &log, DX_LOG_FMT_COMBINED, "203.0.113.7", "GET", "/",
                   "HTTP/1.1", 200, 39, "http://ref.example/", "agent/1.0");
    CHECK(strstr(buf, "\"GET / HTTP/1.1\" 200 39 \"http://ref.example/\" \"agent/1.0\"") != NULL);

    /* Missing values degrade to dashes. */
    memset(buf, 0, sizeof(buf));
    capture_access(buf, sizeof(buf), &log, DX_LOG_FMT_COMBINED, "198.51.100.2", "GET", "/",
                   "HTTP/1.0", 200, 0, NULL, NULL);
    CHECK(strstr(buf, "\"GET / HTTP/1.0\" 200 0 \"-\" \"-\"") != NULL);

    /* Unknown method or target (parse failure) becomes "-". */
    memset(buf, 0, sizeof(buf));
    capture_access(buf, sizeof(buf), &log, DX_LOG_FMT_COMMON, "198.51.100.2", NULL, NULL,
                   "HTTP/1.1", 400, 0, NULL, NULL);
    CHECK(strstr(buf, "\"- - HTTP/1.1\" 400 0") != NULL);

    dx_test_begin("log/access_escaping");

    /* Quotes, backslashes and control characters cannot forge log lines. */
    memset(buf, 0, sizeof(buf));
    capture_access(buf, sizeof(buf), &log, DX_LOG_FMT_COMBINED, "203.0.113.7", "GET",
                   "/a\" b\\c\r\nX-Fake: yes", "HTTP/1.1", 200, 0, NULL, NULL);
    CHECK(strstr(buf, "\\x22") != NULL);   /* the quote is escaped */
    CHECK(strstr(buf, "\\x5c") != NULL);   /* the backslash is escaped */
    CHECK(strstr(buf, "\\x0d\\x0a") != NULL); /* CRLF is escaped */
    /* The raw newline never appears except as the final line terminator. */
    CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1u);
    CHECK(strstr(buf, "X-Fake") == NULL || strstr(buf, "\\x0aX-Fake") != NULL);

    /* A NULL log is a no-op. */
    dx_log_access(NULL, DX_LOG_FMT_COMMON, "x", "GET", 3u, "/", 1u, "HTTP/1.1", 200, 0u, NULL,
                  NULL);
}

int main(void)
{
    test_levels();
    test_filtering_and_format();
    test_access_log();
    printf("test_log: ");
    return dx_test_summary();
}
