/*
 * Unit tests for src/fcache.c: the per-worker open-file cache.
 *
 * The dangerous parts are ownership and lifetime: a descriptor handed out must
 * never be closed out from under the connection holding it, and a slot must
 * never be replaced while it is pinned. These tests exercise every transition.
 */
#include "testing.h"

#include "fcache.h"
#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Open a real file so the fd is a genuine descriptor the cache can own. */
static int open_temp(const char *path)
{
    return open(path, O_RDONLY | O_CREAT | O_TRUNC, 0600);
}

static void test_disabled_cache(void)
{
    struct stat st;
    int fd;

    dx_test_begin("fcache/disabled");

    /* NULL / zero slots / zero validity all mean "no caching". */
    CHECK(dx_fcache_create(0u, 1000u) == NULL);
    CHECK(dx_fcache_create(8u, 0u) == NULL);
    CHECK(dx_fcache_get(NULL, "a", 0u, &st) == NULL);
    fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(dx_fcache_put(NULL, "a", fd, &st, 0u, NULL) == false);
    close(fd);
    dx_fcache_destroy(NULL);
    CHECK_EQ(dx_fcache_count(NULL), 0u);
}

static void test_hit_miss_and_release(void)
{
    dx_fcache_t *c;
    dx_fcache_entry_t *e;
    struct stat st;
    struct stat got;
    int fd;

    dx_test_begin("fcache/hit_miss_release");

    fd = open_temp("/tmp/dxfcache-hit.txt");
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0);

    c = dx_fcache_create(8u, 60000u);
    CHECK(c != NULL);
    CHECK_EQ(dx_fcache_count(c), 0u);

    /* Miss: the path is not cached yet. */
    CHECK(dx_fcache_get(c, "/tmp/dxfcache-hit.txt", 0u, &got) == NULL);

    /* Put: the cache takes ownership and pins it for the caller. */
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-hit.txt", fd, &st, 0u, &e));
    CHECK(e != NULL);
    CHECK_EQ(e->fd, fd);
    CHECK_EQ(e->refs, 1u);
    CHECK_EQ(dx_fcache_count(c), 1u);

    /* Hit: the caller receives the same descriptor and another pin. */
    e = dx_fcache_get(c, "/tmp/dxfcache-hit.txt", 0u, &got);
    CHECK(e != NULL);
    CHECK_EQ(e->fd, fd);
    CHECK_EQ(e->refs, 2u);
    CHECK(got.st_ino == st.st_ino);
    CHECK(got.st_size == st.st_size);

    /* Unpinning twice leaves the entry live and holding its descriptor. */
    dx_fcache_release(e);
    CHECK_EQ(e->refs, 1u);
    dx_fcache_release(e);
    CHECK_EQ(e->refs, 0u);
    CHECK_EQ(dx_fcache_count(c), 1u);
    CHECK(e->fd >= 0); /* still cached for the next request */

    dx_fcache_destroy(c); /* closes the cached descriptor */
}

static void test_expiry(void)
{
    dx_fcache_t *c;
    dx_fcache_entry_t *e;
    struct stat st;
    struct stat got;
    int fd;

    dx_test_begin("fcache/expiry");

    fd = open_temp("/tmp/dxfcache-exp.txt");
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0);

    c = dx_fcache_create(4u, 100u); /* 100 ms window */
    CHECK(c != NULL);
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-exp.txt", fd, &st, 1000u, &e));
    dx_fcache_release(e);
    CHECK_EQ(dx_fcache_count(c), 1u);

    /* Before the window: a hit. */
    CHECK(dx_fcache_get(c, "/tmp/dxfcache-exp.txt", 1099u, &got) != NULL);
    dx_fcache_release(e);

    /* After the window: a miss, and the descriptor is closed. */
    CHECK(dx_fcache_get(c, "/tmp/dxfcache-exp.txt", 1101u, &got) == NULL);
    CHECK_EQ(dx_fcache_count(c), 0u);
    /* Reading from the old descriptor would prove it was never closed. */
    {
        char buf[4];

        CHECK(read(fd, buf, sizeof(buf)) < 0); /* EBADF: it was closed */
    }
    dx_fcache_destroy(c);
}

static void test_pinned_slot_is_never_replaced(void)
{
    dx_fcache_t *c;
    dx_fcache_entry_t *ea;
    dx_fcache_entry_t *eb;
    struct stat sa;
    struct stat sb;
    struct stat sc;
    dx_fcache_entry_t *ec = NULL;
    int fa;
    int fb;
    int fcd;

    dx_test_begin("fcache/pinned_no_replace");

    fa = open_temp("/tmp/dxfcache-pin-a.txt");
    fb = open_temp("/tmp/dxfcache-pin-b.txt");
    CHECK(fa >= 0 && fb >= 0);
    CHECK(fstat(fa, &sa) == 0 && fstat(fb, &sb) == 0);

    c = dx_fcache_create(1u, 60000u); /* one slot: every path collides */
    CHECK(c != NULL);

    CHECK(dx_fcache_put(c, "/tmp/dxfcache-pin-a.txt", fa, &sa, 0u, &ea));
    CHECK_EQ(ea->refs, 1u);

    /* The same path with a fresh entry is a HIT: the cache keeps its entry,
     * closes the caller's redundant descriptor and hands out another pin. */
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-pin-a.txt", fb, &sb, 0u, &eb));
    CHECK_EQ(ea->refs, 2u);
    CHECK_EQ(ea->fd, fa); /* still the original descriptor */

    /* A different path with the slot pinned by a: the cache must refuse and
     * leave the caller's descriptor alone. */
    fcd = open_temp("/tmp/dxfcache-pin-c.txt");
    CHECK(fcd >= 0);
    CHECK(fstat(fcd, &sc) == 0);
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-pin-c.txt", fcd, &sc, 0u, &ec) == false);
    CHECK_EQ(ea->fd, fa); /* a was not evicted */
    CHECK_EQ(ea->refs, 2u);
    CHECK(fstat(fcd, &sc) == 0); /* the caller still owns the descriptor */

    /* Drop both pins, then the slot can be replaced. */
    dx_fcache_release(ea);
    dx_fcache_release(ea);
    CHECK_EQ(ea->refs, 0u);
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-pin-c.txt", fcd, &sc, 0u, &ec));
    CHECK_EQ(ec->fd, fcd);
    CHECK(ec == ea);       /* one slot: the entry object is reused */
    CHECK_EQ(ea->fd, fcd); /* it now owns c, not a */
    CHECK_EQ(dx_fcache_count(c), 1u);

    dx_fcache_release(ec);
    dx_fcache_destroy(c);
}

static void test_pinned_entry_survives_expiry(void)
{
    dx_fcache_t *c;
    dx_fcache_entry_t *e;
    struct stat st;
    int fd;
    struct stat scratch;

    dx_test_begin("fcache/pinned_survives_expiry");

    fd = open_temp("/tmp/dxfcache-pinexp.txt");
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0);

    c = dx_fcache_create(2u, 100u);
    CHECK(c != NULL);
    CHECK(dx_fcache_put(c, "/tmp/dxfcache-pinexp.txt", fd, &st, 1000u, &e));
    CHECK_EQ(e->refs, 1u); /* pinned: a transfer is using it */

    /* Expired while pinned: the lookup refuses to hand it out again, but it
     * must NOT close a descriptor that is still being streamed. */
    CHECK(dx_fcache_get(c, "/tmp/dxfcache-pinexp.txt", 2000u, &scratch) == NULL);
    CHECK(e->fd >= 0);
    CHECK_EQ(dx_fcache_count(c), 1u);
    {
        char buf[8];

        /* The descriptor is still usable: read() must not fail with EBADF. */
        lseek(fd, 0, SEEK_SET);
        CHECK(read(fd, buf, sizeof(buf)) >= 0);
    }
    CHECK_EQ(e->fd, fd); /* same descriptor, not closed behind our back */

    dx_fcache_release(e);
    dx_fcache_destroy(c);
}

static void test_path_validation(void)
{
    dx_fcache_t *c;
    dx_fcache_entry_t *e;
    struct stat st;
    struct stat scratch;
    char longpath[DX_FCACHE_PATH_MAX + 16u];
    int fd;

    dx_test_begin("fcache/path_validation");

    fd = open_temp("/tmp/dxfcache-path.txt");
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0);

    c = dx_fcache_create(8u, 60000u);
    CHECK(c != NULL);

    CHECK(dx_fcache_put(c, "", fd, &st, 0u, &e) == false); /* empty */
    memset(longpath, 'a', sizeof(longpath) - 1u);
    longpath[sizeof(longpath) - 1u] = '\0';
    CHECK(dx_fcache_put(c, longpath, fd, &st, 0u, &e) == false); /* too long */
    CHECK(dx_fcache_get(c, longpath, 0u, &scratch) == NULL);
    CHECK(dx_fcache_count(c) == 0u);

    /* Refusal must not steal the descriptor: the caller still owns it. */
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0);

    close(fd);
    dx_fcache_destroy(c);
}

int main(void)
{
    test_disabled_cache();
    test_hit_miss_and_release();
    test_expiry();
    test_pinned_slot_is_never_replaced();
    test_pinned_entry_survives_expiry();
    test_path_validation();
    printf("test_fcache: ");
    return dx_test_summary();
}
