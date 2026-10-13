/*
 * Unit tests for src/fdcount.c: the descriptor accounting.
 *
 * The invariant that matters is arithmetic: opened minus closed, per kind, and
 * the sum across kinds. A close without a matching open must never make the
 * total underflow into a huge number.
 */
#include "testing.h"

#include "fdcount.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_kind_names(void)
{
    dx_test_begin("fdcount/kind_names");

    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_CONN), "conn") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_FILE), "file") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_CACHE), "cache") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_LISTEN), "listen") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_EPOLL), "epoll") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_WAKEUP), "wakeup") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_LOG), "log") == 0);
    CHECK(strcmp(dx_fdcount_kind_name(DX_FD_MISC), "misc") == 0);
    /* An out-of-range kind is reported, never read out of bounds. */
    CHECK(strcmp(dx_fdcount_kind_name((dx_fd_kind_t)999), "?") == 0);
}

static void test_open_close_delta(void)
{
    uint64_t before;

    dx_test_begin("fdcount/open_close_delta");

    before = dx_fdcount_total();
    dx_fdcount_opened(DX_FD_MISC);
    dx_fdcount_opened(DX_FD_MISC);
    dx_fdcount_opened(DX_FD_MISC);
    CHECK_EQ(dx_fdcount_total(), before + 3u);
    dx_fdcount_closed(DX_FD_MISC);
    CHECK_EQ(dx_fdcount_total(), before + 2u);
    dx_fdcount_closed(DX_FD_MISC);
    dx_fdcount_closed(DX_FD_MISC);
    CHECK_EQ(dx_fdcount_total(), before);

    /* A stray close must not underflow: the total stays at the floor. */
    dx_fdcount_closed(DX_FD_MISC);
    CHECK_EQ(dx_fdcount_total(), before);
}

static void test_by_kind(void)
{
    uint64_t by_kind[DX_FD_KINDS];
    uint64_t total;

    dx_test_begin("fdcount/by_kind");

    dx_fdcount_opened(DX_FD_CONN);
    dx_fdcount_opened(DX_FD_CONN);
    dx_fdcount_opened(DX_FD_FILE);
    dx_fdcount_by_kind(by_kind);
    CHECK(by_kind[DX_FD_CONN] >= 2u);
    CHECK(by_kind[DX_FD_FILE] >= 1u);

    /* The per-kind sum must equal the reported total. */
    total = 0;
    for (unsigned i = 0; i < (unsigned)DX_FD_KINDS; i++) {
        total += by_kind[i];
    }
    CHECK_EQ(total, dx_fdcount_total());

    /* NULL is accepted and does nothing. */
    dx_fdcount_by_kind(NULL);

    dx_fdcount_closed(DX_FD_CONN);
    dx_fdcount_closed(DX_FD_CONN);
    dx_fdcount_closed(DX_FD_FILE);
}

static void test_actual_counts_real_fds(void)
{
    uint64_t before;
    uint64_t during;
    int fd;

    dx_test_begin("fdcount/actual");

    before = dx_fdcount_actual();
    CHECK(before != (uint64_t)-1); /* /proc is mounted in the test environment */

    fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0);
    during = dx_fdcount_actual();
    CHECK_EQ(during, before + 1u);

    close(fd);
    CHECK_EQ(dx_fdcount_actual(), before);
}

static void test_json_shape(void)
{
    char buf[512];
    size_t n;

    dx_test_begin("fdcount/json");

    n = dx_fdcount_json(buf, sizeof(buf));
    CHECK(n > 0u);
    CHECK(strstr(buf, "\"fds_believed\":") != NULL);
    CHECK(strstr(buf, "\"fds_actual\":") != NULL);
    CHECK(strstr(buf, "\"fds_by_kind\":{") != NULL);
    CHECK(strstr(buf, "\"conn\":") != NULL);
    /* Ends with the closing brace of fds_by_kind, so the caller can append. */
    CHECK(buf[n - 1u] == '}');

    /* Too small a buffer is refused, not truncated into invalid JSON. */
    CHECK_EQ(dx_fdcount_json(buf, 8u), 0u);
    CHECK_EQ(dx_fdcount_json(NULL, sizeof(buf)), 0u);
}

int main(void)
{
    test_kind_names();
    test_open_close_delta();
    test_by_kind();
    test_actual_counts_real_fds();
    test_json_shape();
    printf("test_fdcount: ");
    return dx_test_summary();
}
