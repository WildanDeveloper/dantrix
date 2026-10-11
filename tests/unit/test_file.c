/*
 * Integration tests for src/file.c against a real temporary document root:
 * safe opening, symlink refusal, escape rejection, index files, directory
 * redirects and HTML listing escaping.
 */
#include "testing.h"

#include "file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root_template[] = "/tmp/dantrix-file-XXXXXX";
static char *root;

static void put_file(const char *rel, const char *content)
{
    char path[512];
    FILE *f;

    if (snprintf(path, sizeof(path), "%s/%s", root, rel) < 0) {
        return;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(content, 1u, strlen(content), f);
    (void)fclose(f);
}

static void mkdirs(const char *rel)
{
    char path[512];

    if (snprintf(path, sizeof(path), "%s/%s", root, rel) < 0) {
        return;
    }
    (void)mkdir(path, 0755);
}

static void test_setup(void)
{
    char *made = mkdtemp(root_template);

    root = (made != NULL) ? root_template : NULL;
}

static void test_open_regular(void)
{
    dx_fs_t *fs;
    dx_open_result_t r;
    struct stat st;

    dx_test_begin("file/open_regular");
    mkdirs("sub");
    put_file("hello.txt", "hello");
    put_file("sub/nested.txt", "nested");

    fs = dx_fs_create(root, "index.html", false, false);
    CHECK(fs != NULL);
    if (fs == NULL) {
        return;
    }
    CHECK_EQ(dx_fs_open(fs, "hello.txt", false, NULL, 0u, NULL, NULL, 0u, &r), DX_OPEN_OK);
    CHECK(r.st.st_size == 5);
    CHECK(r.fd >= 0);
    (void)close(r.fd);

    /* Path without a trailing slash on a regular file is fine. */
    CHECK_EQ(dx_fs_open(fs, "sub/nested.txt", false, NULL, 0u, NULL, NULL, 0u, &r), DX_OPEN_OK);
    (void)close(r.fd);

    /* Missing file: 404. */
    CHECK_EQ(dx_fs_open(fs, "missing.txt", false, NULL, 0u, NULL, NULL, 0u, &r), DX_OPEN_NOT_FOUND);

    /*
     * ".." is already removed by the path resolver, so file.c only has to
     * guarantee that it can never leave the root: a path that still tries is
     * refused by the kernel (RESOLVE_BENEATH), while one that stays inside
     * is served normally.
     */
    CHECK_EQ(dx_fs_open(fs, "../escape.txt", false, NULL, 0u, NULL, NULL, 0u, &r),
             DX_OPEN_NOT_FOUND);
    CHECK_EQ(dx_fs_open(fs, "sub/../../escape.txt", false, NULL, 0u, NULL, NULL, 0u, &r),
             DX_OPEN_NOT_FOUND);
    CHECK_EQ(dx_fs_open(fs, "sub/../hello.txt", false, NULL, 0u, NULL, NULL, 0u, &r), DX_OPEN_OK);
    (void)close(r.fd);

    /* A directory without a trailing slash is a redirect, not a file. */
    CHECK_EQ(dx_fs_open(fs, "sub", false, NULL, 0u, NULL, NULL, 0u, &r), DX_OPEN_REDIRECT);

    dx_fs_destroy(fs);
    (void)st;
}

static void test_directory_and_index(void)
{
    dx_fs_t *fs = dx_fs_create(root, "index.html", false, false);
    char location[256];
    size_t len = 0;
    dx_open_result_t r;

    dx_test_begin("file/directory_index");
    if (fs == NULL) {
        CHECK(0);
        return;
    }

    /* Directory without trailing slash -> redirect with a slash Location. */
    mkdirs("dir");
    CHECK_EQ(dx_fs_open(fs, "dir", false, location, sizeof(location), &len, NULL, 0u, &r),
             DX_OPEN_REDIRECT);
    CHECK(strncmp(location, "/dir/", 5u) == 0);
    CHECK_EQ(len, 5u);

    /* Root without trailing slash -> "/./" is normalized by the caller; here
     * the raw name is "." so we just require a redirect. */
    CHECK_EQ(dx_fs_open(fs, ".", false, location, sizeof(location), &len, NULL, 0u, &r),
             DX_OPEN_REDIRECT);
    CHECK(len > 0u);

    /* Directory with a slash but no index, listing off -> forbidden. */
    CHECK_EQ(dx_fs_open(fs, "dir", true, location, sizeof(location), &len, NULL, 0u, &r),
             DX_OPEN_FORBIDDEN);

    /* With an index file present, the index is served. */
    put_file("dir/index.html", "<h1>index</h1>");
    CHECK_EQ(dx_fs_open(fs, "dir", true, location, sizeof(location), &len, NULL, 0u, &r),
             DX_OPEN_OK);
    CHECK(r.fd >= 0);
    (void)close(r.fd);

    /* The root directory's own index is served for "/". */
    put_file("index.html", "<h1>root</h1>");
    CHECK_EQ(dx_fs_open(fs, ".", true, location, sizeof(location), &len, NULL, 0u, &r), DX_OPEN_OK);
    (void)close(r.fd);

    dx_fs_destroy(fs);
}

static void test_symlink_policy(void)
{
    dx_fs_t *strict = dx_fs_create(root, "index.html", false, false);
    dx_fs_t *loose = dx_fs_create(root, "index.html", false, true);
    dx_open_result_t r;

    dx_test_begin("file/symlink");
    CHECK(symlink("/etc/passwd", "/tmp/dantrix-file-link-target") == 0 || errno == EEXIST);
    {
        char link[512];

        if (snprintf(link, sizeof(link), "%s/escape-link", root) > 0) {
            (void)unlink(link);
            CHECK_EQ(symlink("/etc/passwd", link), 0);
        }
    }
    if (strict == NULL || loose == NULL) {
        CHECK(0);
        dx_fs_destroy(strict);
        dx_fs_destroy(loose);
        return;
    }

    /* follow_symlinks = false: a symlink is refused (ELOOP or ENOTDIR). */
    {
        dx_open_t res = dx_fs_open(strict, "escape-link", false, NULL, 0u, NULL, NULL, 0u, &r);

        CHECK(res == DX_OPEN_NOT_FOUND || res == DX_OPEN_FORBIDDEN);
        if (r.fd >= 0) {
            (void)close(r.fd);
        }
    }

    /* follow_symlinks = true: RESOLVE_BENEATH still applies, so a link to
     * /etc/passwd (outside the root) must not be served. */
    {
        dx_open_t res = dx_fs_open(loose, "escape-link", false, NULL, 0u, NULL, NULL, 0u, &r);

        CHECK(res == DX_OPEN_NOT_FOUND || res == DX_OPEN_ERROR);
        if (r.fd >= 0) {
            (void)close(r.fd);
        }
    }

    /* A link that stays inside the root is served when symlinks are allowed
     * and refused when they are not. */
    {
        char link[512];
        dx_open_t strict_res;
        dx_open_t loose_res;

        if (snprintf(link, sizeof(link), "%s/inside-link", root) > 0) {
            (void)unlink(link);
            CHECK_EQ(symlink("hello.txt", link), 0);
        }
        strict_res = dx_fs_open(strict, "inside-link", false, NULL, 0u, NULL, NULL, 0u, &r);
        CHECK(strict_res == DX_OPEN_NOT_FOUND || strict_res == DX_OPEN_FORBIDDEN);
        if (r.fd >= 0) {
            (void)close(r.fd);
        }
        loose_res = dx_fs_open(loose, "inside-link", false, NULL, 0u, NULL, NULL, 0u, &r);
        CHECK_EQ(loose_res, DX_OPEN_OK);
        if (r.fd >= 0) {
            (void)close(r.fd);
        }
    }

    dx_fs_destroy(strict);
    dx_fs_destroy(loose);
}

static void test_listing(void)
{
    dx_fs_t *fs = dx_fs_create(root, "index.html", true, false);
    char body[16384];
    size_t n;

    dx_test_begin("file/listing");
    if (fs == NULL) {
        CHECK(0);
        return;
    }

    mkdirs("list");
    put_file("list/plain.txt", "x");
    mkdirs("list/subdir");

    n = dx_fs_list(fs, "list", body, sizeof(body));
    CHECK(n > 0);
    CHECK(strstr(body, "plain.txt") != NULL);
    CHECK(strstr(body, "subdir") != NULL);
    /* A subdirectory gets a trailing slash in the link. */
    CHECK(strstr(body, "subdir/") != NULL);
    CHECK(strstr(body, "Index of /list") != NULL);
    /* "." and ".." are not listed. */
    CHECK(strstr(body, ">.<") == NULL);
    CHECK(strstr(body, ">..<") == NULL);

    /* Escape check: a name with HTML metacharacters is escaped, and the
     * closing tags of the document stay balanced (no injected markup). */
    put_file("list/a<b>&c.txt", "x");
    n = dx_fs_list(fs, "list", body, sizeof(body));
    CHECK(n > 0);
    CHECK(strstr(body, "a&lt;b&gt;&amp;c.txt") != NULL);
    CHECK(strstr(body, "a<b>") == NULL);

    /* A non-directory yields nothing. */
    CHECK_EQ(dx_fs_list(fs, "list/plain.txt", body, sizeof(body)), 0u);
    /* A tiny buffer fails cleanly. */
    CHECK_EQ(dx_fs_list(fs, "list", body, 8u), 0u);
    /* Guards. */
    CHECK_EQ(dx_fs_list(fs, NULL, body, sizeof(body)), 0u);
    CHECK_EQ(dx_fs_list(fs, "list", NULL, 10u), 0u);

    dx_fs_destroy(fs);
}

static void test_html_escape(void)
{
    char buf[64];

    dx_test_begin("file/html_escape");
    CHECK_EQ(dx_html_escape("<>&\"'", 5u, buf, sizeof(buf)), 24u);
    CHECK(strcmp(buf, "&lt;&gt;&amp;&quot;&#39;") == 0);
    /* Plain text and control bytes. */
    CHECK_EQ(dx_html_escape("abc", 3u, buf, sizeof(buf)), 3u);
    CHECK(strcmp(buf, "abc") == 0);
    CHECK_EQ(dx_html_escape("a\x01\x7f"
                            "b",
                            4u, buf, sizeof(buf)),
             2u);
    CHECK(strcmp(buf, "ab") == 0);
    /* Overflow returns 0 rather than truncating mid-entity. */
    CHECK_EQ(dx_html_escape("<<<<", 4u, buf, 8u), 0u);
    CHECK_EQ(dx_html_escape(NULL, 3u, buf, sizeof(buf)), 0u);
}

static void test_teardown(void)
{
    char cmd[512];

    /* Best-effort recursive removal of the temp root. */
    if (snprintf(cmd, sizeof(cmd), "rm -rf '%s' /tmp/dantrix-file-link-target", root) > 0) {
        CHECK_EQ(system(cmd), 0);
    }
}

int main(void)
{
    test_setup();
    if (root == NULL) {
        printf("test_file: cannot create temp root\n");
        return 1;
    }
    test_open_regular();
    test_directory_and_index();
    test_symlink_policy();
    test_listing();
    test_html_escape();
    test_teardown();
    printf("test_file: ");
    return dx_test_summary();
}