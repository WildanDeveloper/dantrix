/*
 * Unit tests for src/mime.c: the built-in table, charset rules, unknown
 * extensions and custom mime.types parsing.
 */
#include "testing.h"

#include "mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_extension(void)
{
    char ext[16];

    dx_test_begin("mime/extension");

    CHECK_EQ(dx_mime_extension("/a/b/c.HTML", ext, sizeof(ext)), 4u);
    CHECK(strcmp(ext, "html") == 0);
    CHECK_EQ(dx_mime_extension("/index.html", ext, sizeof(ext)), 4u);
    CHECK(strcmp(ext, "html") == 0);
    /* A dot in a directory name is not an extension. */
    CHECK_EQ(dx_mime_extension("/a.b/file", ext, sizeof(ext)), 0);
    CHECK_EQ(dx_mime_extension("/noext", ext, sizeof(ext)), 0);
    CHECK_EQ(dx_mime_extension("/trailing.", ext, sizeof(ext)), 0);
    /* Oversized and NULL inputs are refused, never truncated into a match. */
    CHECK_EQ(dx_mime_extension("/file.aaaaaaaaaaaaaaaaaaaa", ext, sizeof(ext)), 0);
    CHECK_EQ(dx_mime_extension(NULL, ext, sizeof(ext)), 0);
    CHECK_EQ(dx_mime_extension("/a.html", NULL, 8u), 0);
    CHECK_EQ(dx_mime_extension("/a.html", ext, 0u), 0);
}

static void test_builtin_types(void)
{
    dx_mime_t *m = dx_mime_create(NULL, "application/octet-stream");

    dx_test_begin("mime/builtin");
    CHECK(m != NULL);
    if (m == NULL) {
        return;
    }

    CHECK(strcmp(dx_mime_type_for(m, "/index.html"), "text/html") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/s.css"), "text/css") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/s.js"), "text/javascript") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/s.mjs"), "text/javascript") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/d.json"), "application/json") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/i.png"), "image/png") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/i.jpg"), "image/jpeg") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/f.woff2"), "font/woff2") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.wasm"), "application/wasm") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/v.mp4"), "video/mp4") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.mp3"), "audio/mpeg") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/d.pdf"), "application/pdf") == 0);

    /* Case insensitive extension matching. */
    CHECK(strcmp(dx_mime_type_for(m, "/INDEX.HTML"), "text/html") == 0);

    /* Unknown extension and no extension fall back to the default type. */
    CHECK(strcmp(dx_mime_type_for(m, "/file.unknown-ext"), "application/octet-stream") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/README"), "application/octet-stream") == 0);

    dx_mime_destroy(m);
}

static void test_charset_rule(void)
{
    dx_mime_t *m = dx_mime_create(NULL, "application/octet-stream");
    char buf[128];

    dx_test_begin("mime/charset");
    if (m == NULL) {
        CHECK(0);
        return;
    }

    /* text types, JSON and JavaScript carry the charset. */
    CHECK(dx_mime_content_type(m, "/i.html", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "text/html; charset=utf-8") == 0);
    CHECK(dx_mime_content_type(m, "/s.js", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "text/javascript; charset=utf-8") == 0);
    CHECK(dx_mime_content_type(m, "/d.json", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "application/json; charset=utf-8") == 0);
    CHECK(dx_mime_content_type(m, "/d.txt", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "text/plain; charset=utf-8") == 0);

    /* Binary types never get a charset. */
    CHECK(dx_mime_content_type(m, "/i.png", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "image/png") == 0);
    CHECK(dx_mime_content_type(m, "/a.wasm", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "application/wasm") == 0);

    /* A too small buffer fails cleanly instead of truncating the value. */
    CHECK_EQ(dx_mime_content_type(m, "/i.html", buf, 4u), 0u);
    CHECK(buf[0] == '\0');
    CHECK_EQ(dx_mime_content_type(m, "/i.html", NULL, 64u), 0u);

    dx_mime_destroy(m);
}

static void test_custom_file(void)
{
    char path[] = "/tmp/dantrix-mime-XXXXXX";
    char dir[] = "/tmp/dantrix-mime-XXXXXX";
    int fd;
    FILE *f;
    dx_mime_t *m;
    char buf[128];
    const char *content = "# a comment line\n"
                          "\n"
                          "text/x-dantrix dxt dxt2;\n"
                          "application/x-custom   cst   cst2 # trailing comment\n"
                          "text/plain txt;\n" /* overrides a built-in */
                          "text/html html;\n" /* same value, still fine */
                          "bogus-line-without-type;\n"
                          "text/x-noext;\n";

    dx_test_begin("mime/custom_file");

    fd = mkstemp(dir);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    (void)close(fd);

    memcpy(path, dir, sizeof(dir));
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) {
        (void)unlink(dir);
        return;
    }
    CHECK_EQ(fwrite(content, 1u, strlen(content), f), strlen(content));
    (void)fclose(f);

    m = dx_mime_create(path, "application/octet-stream");
    CHECK(m != NULL);
    if (m == NULL) {
        (void)unlink(path);
        (void)unlink(dir);
        return;
    }
    CHECK(dx_mime_custom_count(m) > 0u);

    CHECK(strcmp(dx_mime_type_for(m, "/a.dxt"), "text/x-dantrix") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.dxt2"), "text/x-dantrix") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.cst"), "application/x-custom") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.cst2"), "application/x-custom") == 0);
    /* The custom entry overrides the built-in text/plain. */
    CHECK(strcmp(dx_mime_type_for(m, "/a.txt"), "text/plain") == 0);
    /* Unrelated extensions keep the built-in table. */
    CHECK(strcmp(dx_mime_type_for(m, "/a.html"), "text/html") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.png"), "image/png") == 0);

    /* A custom text type also receives the charset. */
    CHECK(dx_mime_content_type(m, "/a.dxt", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "text/x-dantrix; charset=utf-8") == 0);

    dx_mime_destroy(m);

    /* A missing file is not fatal: the built-in table still works. */
    m = dx_mime_create("/nonexistent/mime.types", "text/plain");
    CHECK(m != NULL);
    CHECK_EQ(dx_mime_custom_count(m), 0u);
    CHECK(strcmp(dx_mime_type_for(m, "/a.html"), "text/html") == 0);
    CHECK(strcmp(dx_mime_type_for(m, "/a.zzz"), "text/plain") == 0);
    dx_mime_destroy(m);

    /* An empty path argument also means "built-in only". */
    m = dx_mime_create("", "text/plain");
    CHECK(m != NULL);
    CHECK_EQ(dx_mime_custom_count(m), 0u);
    dx_mime_destroy(m);

    /* NULL default type is a programming error and is refused. */
    CHECK(dx_mime_create(NULL, NULL) == NULL);

    (void)unlink(path);
    (void)unlink(dir);
}

/*
 * Regression: a mime.types file that reaches the 1 MiB cap made the reader
 * terminate one byte past its own allocation. The probe below builds files
 * just below, exactly at and just above the cap; under ASan the old code
 * reported heap-buffer-overflow at mime_load_custom.
 */
static void test_size_cap_boundary(void)
{
    static const size_t caps[] = {1024u * 1024u - 1u, 1024u * 1024u, 1024u * 1024u + 1u,
                                  2u * 1024u * 1024u};
    char path[] = "/tmp/dantrix-mime-cap-XXXXXX";
    int fd;

    dx_test_begin("mime/size_cap_boundary");

    fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }

    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        size_t want = caps[c];
        size_t written = 0;
        unsigned int i = 0;
        FILE *f = fdopen(dup(fd), "wb");
        dx_mime_t *m;

        CHECK(f != NULL);
        if (f == NULL) {
            continue;
        }
        /* Fixed length lines so the file lands on or just past the cap. */
        while (written < want) {
            char line[32];
            int n = snprintf(line, sizeof(line), "text/plain e%06u;\n", i);

            if (n <= 0) {
                break;
            }
            (void)fwrite(line, 1u, (size_t)n, f);
            written += (size_t)n;
            i++;
        }
        (void)fclose(f);

        m = dx_mime_create(path, "application/octet-stream");
        CHECK(m != NULL); /* no overflow, no failure */
        if (m != NULL) {
            char ct[64];

            CHECK(dx_mime_custom_count(m) > 0u);
            (void)dx_mime_content_type(m, "/x.e000001", ct, sizeof(ct));
            CHECK(strcmp(ct, "text/plain; charset=utf-8") == 0);
            dx_mime_destroy(m);
        }
    }
    (void)close(fd);
    (void)unlink(path);
}

/* A tiny output buffer must fail cleanly instead of emitting a partial type. */
static void test_content_type_overflow(void)
{
    dx_mime_t *m = dx_mime_create(NULL, "application/octet-stream");
    char tiny[8];

    dx_test_begin("mime/content_type_overflow");
    CHECK(m != NULL);
    if (m == NULL) {
        return;
    }
    CHECK_EQ(dx_mime_content_type(m, "/a.html", tiny, sizeof(tiny)), 0u);
    CHECK_EQ(tiny[0], '\0');
    CHECK(dx_mime_content_type(m, "/a.html", NULL, 10u) == 0u);
    /* A NULL set falls back to the built-in default, never a crash. */
    {
        char fallback[64];

        CHECK(dx_mime_content_type(NULL, "/a.bin", fallback, sizeof(fallback)) > 0u);
        CHECK(strcmp(fallback, "application/octet-stream") == 0);
        /* Too small for the fallback too: reported as "did not fit". */
        CHECK_EQ(dx_mime_content_type(NULL, "/a.bin", tiny, sizeof(tiny)), 0u);
    }
    dx_mime_destroy(m);
}

int main(void)
{
    test_size_cap_boundary();
    test_content_type_overflow();
    test_extension();
    test_builtin_types();
    test_charset_rule();
    test_custom_file();
    printf("test_mime: ");
    return dx_test_summary();
}
