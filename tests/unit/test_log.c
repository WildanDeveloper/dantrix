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

int main(void)
{
    test_levels();
    test_filtering_and_format();
    printf("test_log: ");
    return dx_test_summary();
}
