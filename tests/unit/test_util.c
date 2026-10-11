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

int main(void)
{
    test_strlcpy();
    test_strlcat();
    test_parse_int();
    test_write_all();
    printf("test_util: ");
    return dx_test_summary();
}
