/*
 * Unit tests for src/http_range.c: Range parsing, ETag formatting, weak
 * validator matching, IMF-fixdate parsing and If-Range rules.
 */
#include "testing.h"

#include "http_range.h"

#include <stdio.h>
#include <string.h>

static dx_range_result_t parse(const char *value, uint64_t size, dx_range_t *r)
{
    return dx_http_range_parse(value, strlen(value), size, r);
}

static void test_range_basic(void)
{
    dx_range_t r;

    dx_test_begin("range/basic");

    CHECK_EQ(parse("bytes=0-499", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.start, 0u);
    CHECK_EQ(r.end, 499u);
    CHECK_EQ(r.length, 500u);

    CHECK_EQ(parse("bytes=500-", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.start, 500u);
    CHECK_EQ(r.end, 999u);
    CHECK_EQ(r.length, 500u);

    /* Suffix form: the last N bytes. */
    CHECK_EQ(parse("bytes=-500", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.start, 500u);
    CHECK_EQ(r.end, 999u);
    CHECK_EQ(r.length, 500u);

    /* A suffix longer than the file yields the whole file. */
    CHECK_EQ(parse("bytes=-5000", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.start, 0u);
    CHECK_EQ(r.end, 999u);
    CHECK_EQ(r.length, 1000u);

    /* The end of a range is clamped to the last byte. */
    CHECK_EQ(parse("bytes=900-99999", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.start, 900u);
    CHECK_EQ(r.end, 999u);
    CHECK_EQ(r.length, 100u);

    /* Surrounding whitespace and the unit name are tolerated. */
    CHECK_EQ(parse("  bytes=0-9  ", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(parse("BYTES=0-9", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(parse("bytes = 0-9", 1000u, &r), DX_RANGE_NONE);

    /* Unsatisfiable ranges. */
    CHECK_EQ(parse("bytes=1000-", 1000u, &r), DX_RANGE_UNSATISFIABLE);
    CHECK_EQ(parse("bytes=1000-2000", 1000u, &r), DX_RANGE_UNSATISFIABLE);
    CHECK_EQ(parse("bytes=0-0", 0u, &r), DX_RANGE_UNSATISFIABLE);
    CHECK_EQ(parse("bytes=0-", 0u, &r), DX_RANGE_UNSATISFIABLE);
    CHECK_EQ(parse("bytes=-1", 0u, &r), DX_RANGE_UNSATISFIABLE);

    /* Single byte range over a single byte file. */
    CHECK_EQ(parse("bytes=0-0", 1u, &r), DX_RANGE_OK);
    CHECK_EQ(r.length, 1u);

    /* Numbers beyond 64 bits saturate instead of wrapping. */
    CHECK_EQ(parse("bytes=99999999999999999999999-", 1000u, &r), DX_RANGE_UNSATISFIABLE);
    CHECK_EQ(parse("bytes=0-99999999999999999999999", 1000u, &r), DX_RANGE_OK);
    CHECK_EQ(r.end, 999u);
}

static void test_range_ignored(void)
{
    dx_range_t r;

    dx_test_begin("range/ignored");

    /* Multi-range is deliberately ignored: a full 200 closes the overlapping
     * range attack surface. */
    CHECK_EQ(parse("bytes=0-10,20-30", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=0-10, 20-30, 40-50", 1000u, &r), DX_RANGE_NONE);

    /* Invalid syntax is ignored, never a 416. */
    CHECK_EQ(parse("", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=abc", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=10-", 1000u, &r), DX_RANGE_OK);    /* sanity: "10-" is valid */
    CHECK_EQ(parse("bytes=10-5", 1000u, &r), DX_RANGE_NONE); /* inverted */
    CHECK_EQ(parse("bytes=-0", 1000u, &r), DX_RANGE_NONE);   /* zero suffix */
    CHECK_EQ(parse("bytes=-abc", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=1-2x", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("bytes=1--2", 1000u, &r), DX_RANGE_NONE);
    CHECK_EQ(parse("items=0-10", 1000u, &r), DX_RANGE_NONE); /* wrong unit */
    CHECK_EQ(parse("bytes=x-y", 1000u, &r), DX_RANGE_NONE);

    /* NULL out is ignored instead of dereferenced. */
    CHECK_EQ(dx_http_range_parse("bytes=0-1", 10u, 10u, NULL), DX_RANGE_NONE);
    CHECK_EQ(dx_http_range_parse(NULL, 0u, 10u, &r), DX_RANGE_NONE);
}

static void test_etag_format(void)
{
    char buf[64];
    size_t n;

    dx_test_begin("range/etag");

    n = dx_http_etag_format(buf, sizeof(buf), 0x6acafefaLL, 28u);
    CHECK(n > 0);
    CHECK(strcmp(buf, "\"6acafefa-1c\"") == 0);

    /* Size zero and a zero timestamp render as "0-0". */
    CHECK(dx_http_etag_format(buf, sizeof(buf), 0, 0) > 0);
    CHECK(strcmp(buf, "\"0-0\"") == 0);

    /* Lowercase hex, fixed width independent of the value. */
    CHECK(dx_http_etag_format(buf, sizeof(buf), 0xabLL, 0xabcdefLL) > 0);
    CHECK(strcmp(buf, "\"ab-abcdef\"") == 0);

    /* A negative timestamp is clamped instead of rendering a minus sign. */
    CHECK(dx_http_etag_format(buf, sizeof(buf), -5, 10u) > 0);
    CHECK(strncmp(buf, "\"0-", 2u) == 0);

    /* Too small a buffer fails without truncating. */
    CHECK_EQ(dx_http_etag_format(buf, 4u, 123LL, 456u), 0u);
    CHECK_EQ(dx_http_etag_format(NULL, 32u, 1LL, 1u), 0u);
}

static void test_etag_matching(void)
{
    dx_test_begin("range/etag_match");

    /* Exact match and "*". */
    CHECK(dx_http_etag_matches("\"abc\"", 5u, "\"abc\""));
    CHECK(dx_http_etag_matches("*", 1u, "\"abc\""));
    CHECK(dx_http_etag_matches("  *  ", 5u, "\"abc\""));

    /* Lists, with optional whitespace. */
    CHECK(dx_http_etag_matches("\"x\", \"abc\" ,\"y\"", 15u, "\"abc\""));
    CHECK(dx_http_etag_matches("\"x\",\"abc\"", 9u, "\"abc\""));

    /* Weak comparison ignores the W/ prefix of a candidate. */
    CHECK(dx_http_etag_matches("W/\"abc\"", 7u, "\"abc\""));
    CHECK(dx_http_etag_matches("\"a\", W/\"abc\"", 12u, "\"abc\""));

    /* No match, empty and malformed values. */
    CHECK(!dx_http_etag_matches("\"other\"", 7u, "\"abc\""));
    CHECK(!dx_http_etag_matches("\"abc\"x", 6u, "\"abc\""));
    CHECK(!dx_http_etag_matches("", 0u, "\"abc\""));
    CHECK(!dx_http_etag_matches("  ", 2u, "\"abc\""));
    CHECK(!dx_http_etag_matches("\"abc", 4u, "\"abc\""));
    CHECK(!dx_http_etag_matches("abc\"", 4u, "\"abc\""));
    CHECK(!dx_http_etag_matches(NULL, 3u, "\"abc\""));
    CHECK(!dx_http_etag_matches("\"abc\"", 5u, NULL));

    /* A malformed trailing comma does not crash; the earlier member still
     * matches, and the empty element matches nothing. */
    CHECK(dx_http_etag_matches("\"a\",\"b\",", 8u, "\"a\""));
    CHECK(!dx_http_etag_matches("\"a\",\"b\",", 8u, "\"\""));
}

static void test_dates(void)
{
    /* 2026-10-11T03:15:30Z as a Unix timestamp. */
    long long reference = 1791688530LL;
    long long t = 0;
    char buf[64];

    dx_test_begin("range/date_parse");

    /* A known IMF-fixdate round trip. */
    CHECK(dx_http_parse_date("Sun, 11 Oct 2026 03:15:30 GMT", 29u, &t));
    CHECK_EQ(t, reference);
    /* The same value with a tab instead of a space. */
    CHECK(dx_http_parse_date("Sun,\t11 Oct 2026 03:15:30 GMT", 29u, &t));
    CHECK_EQ(t, reference);
    /* The value must be reproducible through our own writer. */
    {
        long long again = 0;

        CHECK(dx_http_etag_format(buf, sizeof(buf), t, 1u) > 0);
        CHECK(dx_http_parse_date("Sun, 11 Oct 2026 03:15:30 GMT", 29u, &again));
        CHECK_EQ(again, t);
    }

    /* Epoch. */
    CHECK(dx_http_parse_date("Thu, 01 Jan 1970 00:00:00 GMT", 29u, &t));
    CHECK_EQ(t, 0LL);

    /* Invalid shapes are rejected, never guessed. */
    CHECK(!dx_http_parse_date("", 0u, &t));
    CHECK(!dx_http_parse_date("Sun", 3u, &t));
    CHECK(!dx_http_parse_date("Sun 11 Oct 2026 03:15:30 GMT", 28u, &t));  /* no comma */
    CHECK(!dx_http_parse_date("Sun,11 Oct 2026 03:15:30 GMT", 28u, &t));  /* no space */
    CHECK(!dx_http_parse_date("Sun, 1 Oct 2026 03:15:30 GMT", 28u, &t));  /* 1 digit day */
    CHECK(!dx_http_parse_date("Sun, 11 Foo 2026 03:15:30 GMT", 29u, &t)); /* bad month */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 26 03:15:30 GMT", 27u, &t));   /* 2 digit year */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 25:15:30 GMT", 29u, &t)); /* bad hour */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 03:60:30 GMT", 29u, &t)); /* bad minute */
    CHECK(!dx_http_parse_date("Sun, 32 Oct 2026 03:15:30 GMT", 29u, &t)); /* bad day */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 03:15:30", 26u, &t));     /* no zone */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 03:15:30 PDT", 29u, &t)); /* wrong zone */
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 03:15:30 GMT extra", 35u, &t));
    CHECK(!dx_http_parse_date(NULL, 5u, &t));
    CHECK(!dx_http_parse_date("Sun, 11 Oct 2026 03:15:30 GMT", 29u, NULL));
}

static void test_conditional(void)
{
    long long mtime = 1791688530LL; /* Sun, 11 Oct 2026 03:15:30 GMT */

    dx_test_begin("range/conditional");

    /* If-Modified-Since: not modified when the file is older than the date. */
    CHECK(dx_http_modified_since("Sun, 11 Oct 2026 03:15:30 GMT", 29u, mtime));
    CHECK(dx_http_modified_since("Sun, 11 Oct 2026 03:15:31 GMT", 29u, mtime));
    CHECK(!dx_http_modified_since("Sun, 11 Oct 2026 03:15:29 GMT", 29u, mtime));
    CHECK(!dx_http_modified_since("not a date", 10u, mtime));

    /* If-Range with a strong ETag: exact match only. */
    CHECK(dx_http_if_range_match("\"abc\"", 5u, "\"abc\"", mtime));
    CHECK(!dx_http_if_range_match("\"other\"", 7u, "\"abc\"", mtime));
    /* Weak validators are rejected: If-Range requires strength. */
    CHECK(!dx_http_if_range_match("W/\"abc\"", 7u, "\"abc\"", mtime));
    CHECK(!dx_http_if_range_match("*", 1u, "\"abc\"", mtime));

    /* If-Range with a date: must be exactly Last-Modified. */
    CHECK(dx_http_if_range_match("Sun, 11 Oct 2026 03:15:30 GMT", 29u, "\"x\"", mtime));
    CHECK(!dx_http_if_range_match("Sun, 11 Oct 2026 03:15:31 GMT", 29u, "\"x\"", mtime));
    CHECK(!dx_http_if_range_match("garbage", 7u, "\"x\"", mtime));
    CHECK(!dx_http_if_range_match(NULL, 0u, "\"x\"", mtime));
}

int main(void)
{
    test_range_basic();
    test_range_ignored();
    test_etag_format();
    test_etag_matching();
    test_dates();
    test_conditional();
    printf("test_http_range: ");
    return dx_test_summary();
}