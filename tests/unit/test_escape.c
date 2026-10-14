/*
 * Unit tests for the escaping helpers in src/file.c.
 *
 * These are the functions the security claims rest on: a file name becomes HTML
 * in a directory listing, and a request target becomes the Location of a
 * redirect. The fuzz target (fuzz/fuzz_escape.c) checks the invariants over
 * arbitrary bytes; this file pins the exact expected output for the cases a
 * reader would ask about.
 */
#include "testing.h"

#include "file.h"

#include <stdio.h>
#include <string.h>

static void test_html_escape(void)
{
    char out[256];

    dx_test_begin("escape/html");

    /* The five characters that break out of text or an attribute. */
    CHECK_EQ(dx_html_escape("<", 1, out, sizeof(out)), 4u);
    CHECK(strcmp(out, "&lt;") == 0);
    CHECK_EQ(dx_html_escape(">", 1, out, sizeof(out)), 4u);
    CHECK(strcmp(out, "&gt;") == 0);
    CHECK_EQ(dx_html_escape("&", 1, out, sizeof(out)), 5u);
    CHECK(strcmp(out, "&amp;") == 0);
    CHECK_EQ(dx_html_escape("\"", 1, out, sizeof(out)), 6u);
    CHECK(strcmp(out, "&quot;") == 0);
    CHECK_EQ(dx_html_escape("'", 1, out, sizeof(out)), 5u);
    CHECK(strcmp(out, "&#39;") == 0);

    /* An attack string from the SECURITY.md claim. */
    {
        static const char attack[] = "<script>alert('x')</script>";
        size_t n = dx_html_escape(attack, strlen(attack), out, sizeof(out));

        CHECK(n > 0u);
        CHECK(strstr(out, "<") == NULL);
        CHECK(strstr(out, ">") == NULL);
        CHECK(strstr(out, "'") == NULL);
        CHECK(strstr(out, "&lt;script&gt;") != NULL);
    }

    /* Plain text and the ampersand entity form pass through unchanged. */
    CHECK_EQ(dx_html_escape("plain.txt", 9, out, sizeof(out)), 9u);
    CHECK(strcmp(out, "plain.txt") == 0);
    /* "&amp;" is five characters: the leading '&' expands to "&amp;" (5 bytes)
     * and the remaining "amp;" passes through, so the result is 9 bytes. */
    CHECK_EQ(dx_html_escape("&amp;", 5, out, sizeof(out)), 9u);
    CHECK(strcmp(out, "&amp;amp;") == 0);

    /* A truncation is refused (0), never a partial escape. */
    CHECK_EQ(dx_html_escape("<<<<", 4, out, 4u), 0u);
    /* Exactly enough room: 4 chars "&lt;" needs 5 bytes including the NUL. */
    CHECK_EQ(dx_html_escape("<", 1, out, 5u), 4u);
    CHECK_EQ(dx_html_escape("<", 1, out, 4u), 0u);

    /* NULL and empty inputs. */
    CHECK_EQ(dx_html_escape(NULL, 5, out, sizeof(out)), 0u);
    CHECK_EQ(dx_html_escape("x", 1, NULL, sizeof(out)), 0u);
    CHECK_EQ(dx_html_escape("x", 1, out, 0u), 0u);
    /* Zero length writes nothing and reports nothing. */
    CHECK_EQ(dx_html_escape("abc", 0, out, sizeof(out)), 0u);
}

/*
 * The round-trip property the fuzz harness relies on, checked on fixed inputs so
 * a regression is caught by `make test` and not only by a fuzzing campaign.
 *
 * A wrong entity (mapping '<' to "&gt;") still produces output with no raw
 * quote character, so only decoding the escaped text and comparing it with the
 * input proves the escape is the RIGHT one. That gap is exactly what the
 * original fuzz invariant missed, which is why it is pinned here as well.
 */
static void test_html_round_trip(void)
{
    static const char *const cases[] = {
        "<",
        ">",
        "&",
        "\"",
        "'",
        "<>",
        "&amp;",
        "a<b>c",
        "100% & more",
        "plain.txt",
        "mixed <tag attr=\"v\"> & 'q'</tag>",
        "<<<>>>&&&",
        "\x01\x02",
        "tab\there",
    };
    char esc[512];
    char dec[512];

    dx_test_begin("escape/html_round_trip");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *in = cases[i];
        size_t in_len = strlen(in);
        size_t n = dx_html_escape(in, in_len, esc, sizeof(esc));

        /*
         * An input made only of control bytes escapes to nothing: the function
         * drops control bytes by design, so an empty result is correct there and
         * the round-trip below still has to agree (0 in, 0 out).
         */
        /* Decode by hand: the five entities are the only ones emitted, so any
         * other '&' sequence means the function invented one. */
        size_t o = 0;
        bool ok = true;
        for (size_t k = 0; k < n;) {
            if (esc[k] != '&') {
                dec[o++] = esc[k++];
                continue;
            }
            if (strncmp(esc + k, "&lt;", 4) == 0) {
                dec[o++] = '<';
                k += 4;
            } else if (strncmp(esc + k, "&gt;", 4) == 0) {
                dec[o++] = '>';
                k += 4;
            } else if (strncmp(esc + k, "&amp;", 5) == 0) {
                dec[o++] = '&';
                k += 5;
            } else if (strncmp(esc + k, "&quot;", 6) == 0) {
                dec[o++] = '"';
                k += 6;
            } else if (strncmp(esc + k, "&#39;", 5) == 0) {
                dec[o++] = '\'';
                k += 5;
            } else {
                ok = false;
                break;
            }
        }
        CHECK(ok);
        /* Control bytes are dropped by design, so compare only the kept ones. */
        size_t kept = 0;
        char expect[512];
        for (size_t k = 0; k < in_len; k++) {
            unsigned char c = (unsigned char)in[k];

            if (c < 0x20u || c == 0x7fu) {
                continue;
            }
            expect[kept++] = (char)c;
        }
        CHECK_EQ(o, kept);
        CHECK(memcmp(dec, expect, kept) == 0);
        /*
         * n is the length of the ESCAPED text, which is longer than the input
         * whenever an entity was emitted, so it is not compared with kept. What
         * must hold is that the escaped text is never shorter than the content
         * it represents.
         */
        CHECK(n >= kept);
    }
}

static void test_url_encode(void)
{
    char out[256];

    dx_test_begin("escape/url");

    /* Unreserved characters and '/' survive. */
    CHECK_EQ(dx_url_encode("abcXYZ019-_.~/", 14, out, sizeof(out)), 14u);
    CHECK(strcmp(out, "abcXYZ019-_.~/") == 0);

    /* Everything else becomes a percent escape, uppercase hex. */
    CHECK_EQ(dx_url_encode(" ", 1, out, sizeof(out)), 3u);
    CHECK(strcmp(out, "%20") == 0);
    CHECK_EQ(dx_url_encode("\r\n", 2, out, sizeof(out)), 6u);
    CHECK(strcmp(out, "%0D%0A") == 0);
    CHECK_EQ(dx_url_encode("<", 1, out, sizeof(out)), 3u);
    CHECK(strcmp(out, "%3C") == 0);
    /* A non-ASCII byte is escaped from its raw value. */
    CHECK_EQ(dx_url_encode("\xff", 1, out, sizeof(out)), 3u);
    CHECK(strcmp(out, "%FF") == 0);
    /* '#' and '?' must be escaped so they cannot start a fragment or a query. */
    CHECK_EQ(dx_url_encode("#?", 2, out, sizeof(out)), 6u);
    CHECK(strcmp(out, "%23%3F") == 0);

    /* The header-injection claim: no CR or LF survives. */
    {
        static const char attack[] = "/a\r\nX-Injected: yes";
        size_t n = dx_url_encode(attack, strlen(attack), out, sizeof(out));

        CHECK(n > 0u);
        CHECK(strstr(out, "\r") == NULL);
        CHECK(strstr(out, "\n") == 0);
        CHECK(strstr(out, "%0D%0A") != NULL);
    }

    /* A truncation is refused, and the refusal is not a partial escape. */
    CHECK_EQ(dx_url_encode(" ", 1, out, 3u), 0u); /* needs 4 bytes with the NUL */
    CHECK_EQ(dx_url_encode(" ", 1, out, 4u), 3u);
    CHECK_EQ(dx_url_encode("a", 1, out, 2u), 1u);
    CHECK_EQ(dx_url_encode("a", 1, out, 1u), 0u);
    /* A trailing percent escape that would not fit is refused. */
    CHECK_EQ(dx_url_encode("ab ", 3, out, 5u), 0u);
    CHECK_EQ(dx_url_encode("ab ", 3, out, 6u), 5u);

    /* Empty input is a valid empty output. */
    CHECK_EQ(dx_url_encode("", 0, out, sizeof(out)), 0u);
}

int main(void)
{
    test_html_escape();
    test_html_round_trip();
    test_url_encode();
    printf("test_escape: ");
    return dx_test_summary();
}
