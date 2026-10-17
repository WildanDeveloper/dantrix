/* Unit tests for src/config.c (registry, file/env/override parsing, defaults). */
#include "testing.h"

#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Write text into a temp file and return its path in out (caller unlinks). */
static void write_file(char *out, size_t cap, const char *text)
{
    int fd;
    FILE *f;

    snprintf(out, cap, "/tmp/dantrix-cfg-XXXXXX");
    fd = mkstemp(out);
    if (fd < 0) {
        out[0] = '\0';
        return;
    }
    f = fdopen(fd, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    } else {
        close(fd); /* fdopen failed: the raw descriptor would leak */
    }
}

static void test_defaults(void)
{
    dx_settings_t s;

    dx_test_begin("config/defaults");

    dx_config_defaults(&s);
    CHECK_EQ(s.port, 8080);
    CHECK_EQ(s.workers, 1);
    CHECK_EQ(s.backlog, 4096);
    CHECK(s.reuseport);
    CHECK_EQ((int)s.max_connections, 16384);
    CHECK(!s.cpu_affinity);
    CHECK_EQ((long long)s.header_timeout_ms, 10000);
    CHECK_EQ((long long)s.max_body_size, 1024 * 1024);
    CHECK(strcmp(s.bind, "0.0.0.0") == 0);
    CHECK(strcmp(s.root, "./public") == 0);
    CHECK(strcmp(s.index, "index.html") == 0);
    CHECK(strcmp(s.access_log, "off") == 0);
    CHECK(strcmp(s.error_log, "stderr") == 0);
    CHECK(s.log_level == DX_LOG_INFO);
    CHECK(s.log_format == DX_LOG_FMT_COMBINED);
    CHECK_EQ((int)s.add_header_count, 0);
    CHECK(s.status_path[0] == '\0');
}

static void test_file_basic(void)
{
    char path[64];
    dx_settings_t s;
    FILE *msg = tmpfile();

    dx_test_begin("config/file_basic");

    dx_config_defaults(&s);
    write_file(path, sizeof(path),
               "# comment\n"
               "port = 9000\n"
               "workers = 4            # inline comment\n"
               "  bind   =   127.0.0.1  \n"
               "sendfile = off\n"
               "max_body_size = 2m\n"
               "log_level = debug\n");
    CHECK_EQ(dx_config_file(&s, path, msg), 0);
    CHECK_EQ(s.port, 9000);
    CHECK_EQ(s.workers, 4);
    CHECK(strcmp(s.bind, "127.0.0.1") == 0);
    CHECK(!s.sendfile);
    CHECK_EQ((long long)s.max_body_size, 2 * 1024 * 1024);
    CHECK(s.log_level == DX_LOG_DEBUG);
    unlink(path);
    fclose(msg);
}

static void test_file_quotes_and_errors(void)
{
    char path[64];
    dx_settings_t s;
    FILE *msg = tmpfile();

    dx_test_begin("config/file_errors");

    /* Quoted string with an escaped quote and a space inside. */
    dx_config_defaults(&s);
    write_file(path, sizeof(path), "cache_control = \"public, \\\"x\\\"\"\n");
    CHECK_EQ(dx_config_file(&s, path, msg), 0);
    CHECK(strcmp(s.cache_control, "public, \"x\"") == 0);
    unlink(path);

    /* Unknown key is an error. */
    dx_config_defaults(&s);
    write_file(path, sizeof(path), "not_a_key = 1\n");
    CHECK_EQ(dx_config_file(&s, path, msg), -1);
    unlink(path);

    /* Malformed line (no '='). */
    dx_config_defaults(&s);
    write_file(path, sizeof(path), "port 9000\n");
    CHECK_EQ(dx_config_file(&s, path, msg), -1);
    unlink(path);

    /* Out-of-range value. */
    dx_config_defaults(&s);
    write_file(path, sizeof(path), "port = 70000\n");
    CHECK_EQ(dx_config_file(&s, path, msg), -1);
    unlink(path);

    /* Bad boolean. */
    dx_config_defaults(&s);
    write_file(path, sizeof(path), "sendfile = maybe\n");
    CHECK_EQ(dx_config_file(&s, path, msg), -1);
    unlink(path);

    /* Missing file. */
    CHECK_EQ(dx_config_file(&s, "/nonexistent/dantrix.conf", msg), -1);

    fclose(msg);
}

static void test_repeated_add_header(void)
{
    char path[64];
    dx_settings_t s;
    FILE *msg = tmpfile();

    dx_test_begin("config/add_header");

    dx_config_defaults(&s);
    write_file(path, sizeof(path),
               "add_header = X-Frame-Options: DENY\n"
               "add_header = Referrer-Policy: no-referrer\n");
    CHECK_EQ(dx_config_file(&s, path, msg), 0);
    CHECK_EQ((int)s.add_header_count, 2);
    CHECK(strcmp(s.add_header[0], "X-Frame-Options: DENY") == 0);
    CHECK(strcmp(s.add_header[1], "Referrer-Policy: no-referrer") == 0);
    unlink(path);
    fclose(msg);
}

static void test_override(void)
{
    dx_settings_t s;
    FILE *msg = tmpfile();

    dx_test_begin("config/override");

    dx_config_defaults(&s);
    CHECK_EQ(dx_config_override(&s, "port=9001", msg), 0);
    CHECK_EQ(s.port, 9001);
    CHECK_EQ(dx_config_override(&s, "keepalive_timeout=60", msg), 0);
    CHECK_EQ((long long)s.keepalive_timeout_ms, 60000);

    /* Without '=' it is an error. */
    CHECK_EQ(dx_config_override(&s, "port", msg), -1);
    /* Unknown key is an error. */
    CHECK_EQ(dx_config_override(&s, "bogus=1", msg), -1);

    fclose(msg);
}

static void test_env(void)
{
    dx_settings_t s;
    FILE *msg = tmpfile();

    dx_test_begin("config/env");

    dx_config_defaults(&s);
    setenv("DANTRIX_PORT", "9999", 1);
    setenv("DANTRIX_WORKERS", "3", 1);
    setenv("DANTRIX_SENDFILE", "false", 1);
    CHECK_EQ(dx_config_env(&s, msg), 0);
    CHECK_EQ(s.port, 9999);
    CHECK_EQ(s.workers, 3);
    CHECK(!s.sendfile);

    /* A malformed environment value is an error. */
    setenv("DANTRIX_PORT", "notaport", 1);
    CHECK_EQ(dx_config_env(&s, msg), -1);

    unsetenv("DANTRIX_PORT");
    unsetenv("DANTRIX_WORKERS");
    unsetenv("DANTRIX_SENDFILE");
    fclose(msg);
}

static void test_validate(void)
{
    dx_settings_t s;
    FILE *msg = tmpfile();
    char dir[] = "/tmp/dantrix-cfgdir-XXXXXX";

    dx_test_begin("config/validate");

    CHECK(mkdtemp(dir) != NULL);

    dx_config_defaults(&s);
    dx_strlcpy(s.root, dir, sizeof(s.root));
    CHECK_EQ(dx_config_validate(&s, msg), 0);
    CHECK_EQ(dx_config_check_root(&s, msg), 0);

    /* max_header_size < max_request_line is rejected by the syntax check. */
    s.max_header_size = 1024;
    s.max_request_line = 2048;
    CHECK_EQ(dx_config_validate(&s, msg), -1);
    s.max_header_size = 16384;
    s.max_request_line = 8192;

    /* A missing root is a filesystem error, not a syntax error. */
    dx_strlcpy(s.root, "/nonexistent/root/xyz", sizeof(s.root));
    CHECK_EQ(dx_config_validate(&s, msg), 0);
    CHECK_EQ(dx_config_check_root(&s, msg), -1);

    rmdir(dir);
    fclose(msg);
}

static void test_print(void)
{
    dx_settings_t s;
    char path[64];
    FILE *out;

    dx_test_begin("config/print");

    dx_config_defaults(&s);
    dx_strlcpy(s.root, "/tmp", sizeof(s.root));
    dx_config_override(&s, "add_header=X-Test: 1", stderr);

    snprintf(path, sizeof(path), "/tmp/dantrix-print-%d", (int)getpid());
    out = fopen(path, "w");
    CHECK(out != NULL);
    if (out != NULL) {
        dx_config_print(&s, out);
        fclose(out);
        out = fopen(path, "r");
        if (out != NULL) {
            char buf[8192];
            size_t n = fread(buf, 1u, sizeof(buf) - 1u, out);

            buf[n] = '\0';
            CHECK(strstr(buf, "port = 8080") != NULL);
            CHECK(strstr(buf, "add_header = X-Test: 1") != NULL);
            fclose(out);
        }
        unlink(path);
    }
}

static void test_long_values(void)
{
    dx_settings_t s;
    FILE *msg = tmpfile();
    char path[DX_CFG_PATH_MAX];
    size_t i;

    dx_test_begin("config/long_values");

    /* A 4000-byte path must survive dx_config_set() intact, not be truncated
     * to a small scratch buffer (the CLI -o/--root path used to truncate it). */
    dx_config_defaults(&s);
    for (i = 0; i < sizeof(path) - 1u; i++) {
        path[i] = (i == 0) ? '/' : 'a';
    }
    path[sizeof(path) - 1u] = '\0';
    CHECK_EQ(dx_config_set(&s, "root", path, "-o", msg), 0);
    CHECK_EQ(strlen(s.root), strlen(path));
    CHECK(strcmp(s.root, path) == 0);

    /* A value longer than the field is refused, not truncated. */
    dx_config_defaults(&s);
    CHECK_EQ(dx_config_set(&s, "index", path, "-o", msg), -1);

    /* The access log path is DX_CFG_PATH_MAX wide too. */
    dx_config_defaults(&s);
    CHECK_EQ(dx_config_set(&s, "access_log", path, "-o", msg), 0);
    CHECK(strcmp(s.access_log, path) == 0);

    fclose(msg);
}

int main(void)
{
    test_defaults();
    test_file_basic();
    test_file_quotes_and_errors();
    test_repeated_add_header();
    test_override();
    test_env();
    test_validate();
    test_print();
    test_long_values();
    printf("test_config: ");
    return dx_test_summary();
}
