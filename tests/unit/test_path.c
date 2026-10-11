/*
 * Unit tests for src/path.c: query stripping, absolute form, percent-decoding
 * exactly once, normalization, traversal rejection and hidden files.
 */
#include "testing.h"

#include "path.h"

#include <string.h>

static int resolve(const char *target, dx_path_t *out, bool hidden)
{
    dx_path_opts_t opts;

    opts.hidden_files = hidden;
    return dx_path_resolve(target, strlen(target), &opts, out);
}

static void test_basic(void)
{
    dx_path_t p;

    dx_test_begin("path/basic");

    /* Root itself. */
    CHECK_EQ(resolve("/", &p, false), 0);
    CHECK(strcmp(p.path, ".") == 0);
    /* The root always counts as having a trailing slash, so serving its index
     * never triggers a redirect to "/./". */
    CHECK(p.had_trailing_slash == true);

    CHECK_EQ(resolve("/index.html", &p, false), 0);
    CHECK(strcmp(p.path, "index.html") == 0);
    CHECK(p.had_trailing_slash == false);

    CHECK_EQ(resolve("/a/b/c.txt", &p, false), 0);
    CHECK(strcmp(p.path, "a/b/c.txt") == 0);

    /* Query string is dropped, trailing slash is remembered. */
    CHECK_EQ(resolve("/a/b.txt?v=1&x=2", &p, false), 0);
    CHECK(strcmp(p.path, "a/b.txt") == 0);

    CHECK_EQ(resolve("/dir/", &p, false), 0);
    CHECK(strcmp(p.path, "dir") == 0);
    CHECK(p.had_trailing_slash == true);

    /* Absolute form (proxy style) is accepted. */
    CHECK_EQ(resolve("http://example.com/a/b?x=1", &p, false), 0);
    CHECK(strcmp(p.path, "a/b") == 0);
    CHECK_EQ(resolve("https://example.com/", &p, false), 0);
    CHECK(strcmp(p.path, ".") == 0);
    CHECK_EQ(resolve("HTTP://EXAMPLE.COM/x", &p, false), 0);
    CHECK(strcmp(p.path, "x") == 0);
    /* Authority without a path is not a file request. */
    CHECK_EQ(resolve("http://example.com", &p, false), 400);

    /* Asterisk form and other non origin-form targets are rejected. */
    CHECK_EQ(resolve("*", &p, false), 400);
    CHECK_EQ(resolve("", &p, false), 400);
    CHECK_EQ(resolve("relative/path", &p, false), 400);
}

static void test_normalization(void)
{
    dx_path_t p;

    dx_test_begin("path/normalize");

    CHECK_EQ(resolve("//a///b", &p, false), 0);
    CHECK(strcmp(p.path, "a/b") == 0);

    CHECK_EQ(resolve("/./a/./b/", &p, false), 0);
    CHECK(strcmp(p.path, "a/b") == 0);
    CHECK(p.had_trailing_slash == true);

    CHECK_EQ(resolve("/a/b/../c", &p, false), 0);
    CHECK(strcmp(p.path, "a/c") == 0);

    CHECK_EQ(resolve("/a/b/../../c", &p, false), 0);
    CHECK(strcmp(p.path, "c") == 0);

    /* ".." that would escape the root is rejected. */
    CHECK_EQ(resolve("/../etc/passwd", &p, false), 400);
    CHECK_EQ(resolve("/a/../../etc/passwd", &p, false), 400);
    CHECK_EQ(resolve("/..", &p, false), 400);
    CHECK_EQ(resolve("/a/../..", &p, false), 400);

    /* The escape must also be caught after percent-decoding. */
    CHECK_EQ(resolve("/%2e%2e/%2e%2e/etc/passwd", &p, false), 400);
    CHECK_EQ(resolve("/a/%2E%2E/%2E%2E/x", &p, false), 400);
    CHECK_EQ(resolve("/%2e%2e%2fetc/passwd", &p, false), 400);

    /* Decoded separators become real separators, so traversal works there. */
    CHECK_EQ(resolve("/dir%2f..%2f..%2fetc", &p, false), 400);
    /* A harmless encoded slash is still just a segment. */
    CHECK_EQ(resolve("/a%2fb", &p, false), 0);
    CHECK(strcmp(p.path, "a/b") == 0);
}

static void test_percent_decoding(void)
{
    dx_path_t p;

    dx_test_begin("path/decode");

    CHECK_EQ(resolve("/hello%20world.txt", &p, false), 0);
    CHECK(strcmp(p.path, "hello world.txt") == 0);

    /* Plus stays a plus: this is a path, not a query string. */
    CHECK_EQ(resolve("/a+b", &p, false), 0);
    CHECK(strcmp(p.path, "a+b") == 0);

    /* Malformed escapes are rejected, never silently passed through. */
    CHECK_EQ(resolve("/a%2", &p, false), 400);
    CHECK_EQ(resolve("/a%", &p, false), 400);
    CHECK_EQ(resolve("/a%zz", &p, false), 400);
    CHECK_EQ(resolve("/a%2z9", &p, false), 400);

    /* Encoded NUL is rejected before it can truncate a path. */
    CHECK_EQ(resolve("/a%00b", &p, false), 400);
    CHECK_EQ(resolve("/%00", &p, false), 400);

    /* Decoding happens exactly once: %252e decodes to "%2e", a literal name. */
    CHECK_EQ(resolve("/%252e%252e/x", &p, false), 0);
    CHECK(strcmp(p.path, "%2e%2e/x") == 0);

    /* Control characters, encoded or raw, never reach the filesystem. */
    CHECK_EQ(resolve("/a%01b", &p, false), 400);
    CHECK_EQ(resolve("/a%0Ab", &p, false), 400);
    CHECK_EQ(resolve("/a%7Fb", &p, false), 400);
    CHECK_EQ(resolve("/a\x7f"
                     "b",
                     &p, false),
             400);
}

static void test_hidden_files(void)
{
    dx_path_t p;

    dx_test_begin("path/hidden");

    /* Dot segments are 404 while hidden_files is off. */
    CHECK_EQ(resolve("/.env", &p, false), 404);
    CHECK_EQ(resolve("/.git/config", &p, false), 404);
    CHECK_EQ(resolve("/sub/.secret", &p, false), 404);
    CHECK_EQ(resolve("/%2eg%2e%65nv", &p, false), 404);

    /* /.well-known/ stays reachable even when hidden files are refused. */
    CHECK_EQ(resolve("/.well-known/acme.txt", &p, false), 0);
    CHECK(strcmp(p.path, ".well-known/acme.txt") == 0);
    /* Only the exact segment name is exempt (11 characters, no suffix). */
    CHECK_EQ(resolve("/.well-knownx/a", &p, false), 404);
    CHECK_EQ(resolve("/.well-known.other", &p, false), 404);

    /* With hidden_files enabled they resolve normally. */
    CHECK_EQ(resolve("/.env", &p, true), 0);
    CHECK(strcmp(p.path, ".env") == 0);
    CHECK_EQ(resolve("/a/.git/config", &p, true), 0);
    CHECK(strcmp(p.path, "a/.git/config") == 0);
}

static void test_limits_and_guards(void)
{
    dx_path_t p;
    char big[DX_PATH_MAX + 64u];

    dx_test_begin("path/limits");

    /* A path longer than the maximum is rejected, never truncated. */
    memset(big, '/', sizeof(big));
    big[0] = '/';
    big[1] = 'a';
    big[2] = '\0';
    CHECK_EQ(dx_path_resolve(big, DX_PATH_MAX + 8u, NULL, &p), 400);

    /* Exactly at the limit still resolves. */
    {
        size_t n = DX_PATH_MAX - 2u;

        memset(big, 'a', sizeof(big));
        big[0] = '/';
        big[n + 1u] = '\0';
        CHECK_EQ(dx_path_resolve(big, n + 1u, NULL, &p), 0);
        CHECK_EQ(strlen(p.path), n);
    }

    /* NULL arguments are handled. */
    CHECK_EQ(dx_path_resolve(NULL, 4u, NULL, &p), 400);
    CHECK_EQ(dx_path_resolve("/x", 2u, NULL, NULL), 400);

    /* opts == NULL means hidden files are refused (defaults are strict). */
    CHECK_EQ(dx_path_resolve("/.env", 5u, NULL, &p), 404);
}

int main(void)
{
    test_basic();
    test_normalization();
    test_percent_decoding();
    test_hidden_files();
    test_limits_and_guards();
    printf("test_path: ");
    return dx_test_summary();
}
