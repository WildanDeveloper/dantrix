/* Unit tests for src/util.c (bounded strings, strict parsing, safe writes). */
#include "testing.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

static void test_strlcpy(void)
{
    char buf[8];

    dx_test_begin("util/strlcpy");

    memset(buf, 'X', sizeof(buf));
    CHECK_EQ(dx_strlcpy(buf, "abc", sizeof(buf)), 3);
    CHECK(strcmp(buf, "abc") == 0);

    /* Truncation still NUL terminates and reports the full source length. */
    CHECK_EQ(dx_strlcpy(buf, "0123456789", sizeof(buf)), 10);
    CHECK_EQ(strlen(buf), 7);
    CHECK(strcmp(buf, "0123456") == 0);

    /* size == 1 keeps exactly the terminator. */
    CHECK_EQ(dx_strlcpy(buf, "abc", 1), 3);
    CHECK_EQ(buf[0], '\0');

    /* size == 0 must not touch the buffer at all. */
    buf[0] = 'Z';
    CHECK_EQ(dx_strlcpy(buf, "abc", 0), 3);
    CHECK_EQ(buf[0], 'Z');

    /* NULL source is handled instead of crashing. */
    CHECK_EQ(dx_strlcpy(buf, NULL, sizeof(buf)), 0);
    CHECK_EQ(buf[0], '\0');
}

static void test_strlcat(void)
{
    char buf[16];

    dx_test_begin("util/strlcat");

    buf[0] = '\0';
    CHECK_EQ(dx_strlcat(buf, "ab", sizeof(buf)), 2);
    CHECK_EQ(dx_strlcat(buf, "cd", sizeof(buf)), 4);
    CHECK(strcmp(buf, "abcd") == 0);

    /* Appending past the capacity truncates but never overflows. */
    CHECK_EQ(dx_strlcat(buf, "efghijklmnopqrst", sizeof(buf)), 4 + 16);
    CHECK_EQ(strlen(buf), 15);
    CHECK(strncmp(buf, "abcdefghijklmno", 15) == 0);

    /* An unterminated destination must not be written to. */
    {
        char bad[4];
        memset(bad, 'A', sizeof(bad));
        CHECK_EQ(dx_strlcat(bad, "x", sizeof(bad)), 4 + 1);
        CHECK(bad[0] == 'A'); /* untouched */
    }
}

static void test_parse_int(void)
{
    int v = -12345;

    dx_test_begin("util/parse_int");

    CHECK(dx_parse_int("0", 0, 100, &v) && v == 0);
    CHECK(dx_parse_int("42", 0, 100, &v) && v == 42);
    CHECK(dx_parse_int("-7", -10, 10, &v) && v == -7);
    CHECK(dx_parse_int("007", 0, 100, &v) && v == 7);

    /* Out of range. */
    CHECK(!dx_parse_int("101", 0, 100, &v));
    CHECK(!dx_parse_int("-11", -10, 10, &v));

    /* Rejects anything that is not a plain decimal integer. */
    CHECK(!dx_parse_int("", 0, 10, &v));
    CHECK(!dx_parse_int(" 1", 0, 10, &v));
    CHECK(!dx_parse_int("1 ", 0, 10, &v));
    CHECK(!dx_parse_int("+1", 0, 10, &v));
    CHECK(!dx_parse_int("-", 0, 10, &v));
    CHECK(!dx_parse_int("-0", 0, 10, &v));
    CHECK(!dx_parse_int("0x10", 0, 10, &v));
    CHECK(!dx_parse_int("1a", 0, 10, &v));
    CHECK(!dx_parse_int("1.5", 0, 10, &v));
    CHECK(!dx_parse_int(NULL, 0, 10, &v));

    /* INT_MAX and INT_MIN boundaries. */
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", INT_MAX);
        CHECK(dx_parse_int(buf, INT_MIN, INT_MAX, &v) && v == INT_MAX);
        snprintf(buf, sizeof(buf), "%d", INT_MIN);
        CHECK(dx_parse_int(buf, INT_MIN, INT_MAX, &v) && v == INT_MIN);
    }

    /* Overflow far beyond INT_MAX is rejected, not wrapped. */
    CHECK(!dx_parse_int("99999999999999999999", 0, INT_MAX, &v));
}

static void test_write_all(void)
{
    int fd;
    char big[8192];
    memset(big, 'a', sizeof(big));

    dx_test_begin("util/write_all");

    fd = open("/dev/null", O_WRONLY);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    CHECK_EQ(dx_write_all(fd, "hi", 2), 0);
    CHECK_EQ(dx_write_all(fd, big, sizeof(big)), 0); /* exercises the loop */
    CHECK_EQ(dx_write_all(fd, "", 0), 0);
    close(fd);

    /* A closed descriptor is an error, not a silent success. */
    CHECK_EQ(dx_write_all(fd, "x", 1), -1);
}

static void test_parse_ull(void)
{
    unsigned long long v = 0;

    dx_test_begin("util/parse_ull");

    CHECK(dx_parse_ull("0", 1u, &v) && v == 0u);
    CHECK(dx_parse_ull("42", 2u, &v) && v == 42u);
    CHECK(dx_parse_ull("18446744073709551615", 20u, &v) && v == ULLONG_MAX);
    /* One past the maximum overflows and is rejected. */
    CHECK(!dx_parse_ull("18446744073709551616", 20u, &v));
    /* Leading zeros are plain decimals, still fine. */
    CHECK(dx_parse_ull("007", 3u, &v) && v == 7u);
    /* Rejects: empty range, sign, space, junk. */
    CHECK(!dx_parse_ull("", 0u, &v));
    CHECK(!dx_parse_ull("-1", 2u, &v));
    CHECK(!dx_parse_ull(" 1", 2u, &v));
    CHECK(!dx_parse_ull("1 ", 2u, &v));
    CHECK(!dx_parse_ull("0x10", 4u, &v));
    CHECK(!dx_parse_ull("abc", 3u, &v));
    /* NULL arguments. */
    CHECK(!dx_parse_ull(NULL, 1u, &v));
    CHECK(!dx_parse_ull("1", 1u, NULL));
}

static void test_parse_size(void)
{
    size_t v = 0;

    dx_test_begin("util/parse_size");

    CHECK(dx_parse_size("0", &v) && v == 0u);
    CHECK(dx_parse_size("4096", &v) && v == 4096u);
    CHECK(dx_parse_size("8k", &v) && v == 8u * 1024u);
    CHECK(dx_parse_size("8K", &v) && v == 8u * 1024u);
    CHECK(dx_parse_size("1m", &v) && v == 1024u * 1024u);
    CHECK(dx_parse_size("2G", &v) && v == 2ull * 1024ull * 1024ull * 1024ull);
    /* Suffix must be a single character, nothing may follow. */
    CHECK(!dx_parse_size("8kk", &v));
    CHECK(!dx_parse_size("8 k", &v));
    CHECK(!dx_parse_size("k", &v));
    CHECK(!dx_parse_size("", &v));
    CHECK(!dx_parse_size("-1", &v));
    CHECK(!dx_parse_size("8x", &v));
    /* Overflowing either the digits or the multiplied value fails. */
    CHECK(!dx_parse_size("99999999999999999999999", &v));
    CHECK(!dx_parse_size("18446744073709551616g", &v));
    CHECK(!dx_parse_size(NULL, &v));
}

static void test_http_date(void)
{
    char buf[64];
    size_t n;

    dx_test_begin("util/http_date");

    /* 2026-10-11T20:00:00Z is a Sunday: IMF-fixdate from a fixed timestamp. */
    n = dx_http_date(buf, sizeof(buf), 1791748800000LL);
    CHECK(n == 29u);
    CHECK(strcmp(buf, "Sun, 11 Oct 2026 20:00:00 GMT") == 0);

    /* Unix epoch. */
    n = dx_http_date(buf, sizeof(buf), 0LL);
    CHECK(n == 29u);
    CHECK(strcmp(buf, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);

    /* Too small a buffer is rejected, not truncated. */
    CHECK_EQ(dx_http_date(buf, 16u, 0LL), 0u);
    CHECK_EQ(dx_http_date(NULL, sizeof(buf), 0LL), 0u);

    /* when_ms < 0 means "now": it must produce a plausible fixdate. */
    n = dx_http_date(buf, sizeof(buf), -1);
    CHECK(n == 29u);
    CHECK(strstr(buf, " GMT") == buf + n - 4u);
    CHECK(strstr(buf, ", ") != NULL);
}

int main(void)
{
    test_strlcpy();
    test_strlcat();
    test_parse_int();
    test_write_all();
    test_parse_ull();
    test_parse_size();
    test_http_date();
    printf("test_util: ");
    return dx_test_summary();
}
