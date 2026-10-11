/*
 * Unit tests for src/http_parser.c: strict parsing, incremental feeds and
 * every documented error mapping (400/414/431/501/505).
 *
 * Feeding contract reminder: dx_http_parser_feed() takes the request start as
 * the base plus the cumulative length seen so far; the parser keeps its scan
 * position across calls, which is what makes it incremental.
 */
#include "testing.h"

#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy an offset/length field out of the fed window into a NUL string. */
static void field(const unsigned char *base, size_t off, size_t len, char *out, size_t cap)
{
    if (len >= cap) {
        len = cap - 1u;
    }
    memcpy(out, base + off, len);
    out[len] = '\0';
}

static dx_parse_result_t parse_whole(dx_http_parser_t *p, const char *req, size_t *consumed)
{
    dx_http_parser_init(p, NULL);
    return dx_http_parser_feed(p, (const unsigned char *)req, strlen(req), consumed);
}

static void test_simple_get(void)
{
    static const char req[] = "GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\n"
                              "User-Agent: unit-test\r\n\r\n";
    dx_http_parser_t p;
    size_t consumed = 0;
    dx_parse_result_t r;
    char buf[64];
    size_t voff;
    size_t vlen;

    dx_test_begin("http_parser/simple_get");

    r = parse_whole(&p, req, &consumed);
    CHECK_EQ(r, DX_PARSE_OK);
    CHECK_EQ(consumed, sizeof(req) - 1u);
    field((const unsigned char *)req, p.req.method_off, p.req.method_len, buf, sizeof(buf));
    CHECK(strcmp(buf, "GET") == 0);
    field((const unsigned char *)req, p.req.target_off, p.req.target_len, buf, sizeof(buf));
    CHECK(strcmp(buf, "/hello?x=1") == 0);
    CHECK_EQ(p.req.minor_version, 1);
    CHECK_EQ(p.req.num_headers, 2);

    /* Case insensitive lookup with OWS already stripped. */
    CHECK(dx_http_request_find_header(&p.req, (const unsigned char *)req, "HOST", &voff, &vlen));
    field((const unsigned char *)req, voff, vlen, buf, sizeof(buf));
    CHECK(strcmp(buf, "example.com") == 0);
    CHECK(dx_http_request_find_header(&p.req, (const unsigned char *)req, "user-agent", &voff,
                                      &vlen));
    field((const unsigned char *)req, voff, vlen, buf, sizeof(buf));
    CHECK(strcmp(buf, "unit-test") == 0);
    CHECK(dx_http_request_find_header(&p.req, (const unsigned char *)req, "Missing", &voff,
                                      &vlen) == false);
}

static void test_minimal_requests(void)
{
    dx_http_parser_t p;
    size_t consumed = 0;

    dx_test_begin("http_parser/minimal");

    /* HTTP/1.0 needs no Host, so this is the shortest legal request. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.0\r\n\r\n", &consumed), DX_PARSE_OK);
    CHECK_EQ(p.req.minor_version, 0);
    CHECK_EQ(p.req.num_headers, 0);
    CHECK_EQ(consumed, 18u);

    /* HTTP/1.0 with a Host is fine too. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.0\r\nHost: h\r\n\r\n", &consumed), DX_PARSE_OK);

    /* HTTP/1.1 requires exactly one non-empty Host. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.1\r\nHost: h\r\n\r\n", &consumed), DX_PARSE_OK);
    CHECK_EQ(p.req.minor_version, 1);
}

static void test_split_feeds(void)
{
    static const char req[] = "POST-ISH /x HTTP/1.1\r\nHost: a\r\nX-B: c d\r\n\r\n";
    static const size_t chunks[] = {1u, 2u, 3u, 5u, 7u, 13u};
    size_t ci;
    size_t total = sizeof(req) - 1u;

    dx_test_begin("http_parser/split");

    for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
        dx_http_parser_t p;
        size_t step = chunks[ci];
        size_t fed = 0;
        size_t consumed = 0;
        dx_parse_result_t r = DX_PARSE_NEED_DATA;

        dx_http_parser_init(&p, NULL);
        while (fed < total) {
            fed += (total - fed < step) ? (total - fed) : step;
            r = dx_http_parser_feed(&p, (const unsigned char *)req, fed, &consumed);
            if (r != DX_PARSE_NEED_DATA) {
                break;
            }
        }
        CHECK_EQ(r, DX_PARSE_OK);
        CHECK_EQ(consumed, total);
        CHECK_EQ(p.req.method_len, 8u);
        CHECK_EQ(p.req.num_headers, 2);
    }
}

static void test_split_at_crlf_boundary(void)
{
    dx_http_parser_t p;
    size_t consumed = 0;
    dx_parse_result_t r;
    static const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";

    dx_test_begin("http_parser/split_crlf");

    dx_http_parser_init(&p, NULL);
    r = dx_http_parser_feed(&p, (const unsigned char *)req, 15u, &consumed); /* "...1.1\r" */
    CHECK_EQ(r, DX_PARSE_NEED_DATA);
    r = dx_http_parser_feed(&p, (const unsigned char *)req, 24u, &consumed); /* "...x\r" */
    CHECK_EQ(r, DX_PARSE_NEED_DATA);
    r = dx_http_parser_feed(&p, (const unsigned char *)req, 26u, &consumed); /* "... \r\n\r" */
    CHECK_EQ(r, DX_PARSE_NEED_DATA);
    r = dx_http_parser_feed(&p, (const unsigned char *)req, sizeof(req) - 1u, &consumed);
    CHECK_EQ(r, DX_PARSE_OK);
    CHECK_EQ(consumed, sizeof(req) - 1u);
    CHECK_EQ(p.req.num_headers, 1);
}

static void test_header_values(void)
{
    static const char req[] = "GET / HTTP/1.1\r\nHost: \t spaced.example.com \t\r\n\r\n";
    dx_http_parser_t p;
    size_t consumed = 0;
    size_t voff;
    size_t vlen;
    char buf[128];

    dx_test_begin("http_parser/values");

    CHECK_EQ(parse_whole(&p, req, &consumed), DX_PARSE_OK);
    CHECK(dx_http_request_find_header(&p.req, (const unsigned char *)req, "Host", &voff, &vlen));
    field((const unsigned char *)req, voff, vlen, buf, sizeof(buf));
    CHECK(strcmp(buf, "spaced.example.com") == 0);

    /* obs-text (>= 0x80) is allowed in values per RFC 9110. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.1\r\nHost: h\r\nX-N: caf\xc3\xa9\r\n\r\n", &consumed),
             DX_PARSE_OK);

    /* An empty value is legal for ordinary headers. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.1\r\nHost: h\r\nX-E:\r\n\r\n", &consumed), DX_PARSE_OK);
}

static void test_error_table(void)
{
    static const struct {
        const char *req;
        size_t len; /* 0: use strlen; the NUL case needs an explicit length */
        int status;
    } cases[] = {
        /* Bare LF line ending. */
        {"GET / HTTP/1.1\nHost: x\r\n\r\n", 0, 400},
        /* Bare CR inside the request line. */
        {"GET /x\rHTTP/1.1\r\n\r\n", 0, 400},
        /* Double space between method and target. */
        {"GET  / HTTP/1.1\r\n\r\n", 0, 400},
        /* Missing version (HTTP/0.9 style). */
        {"GET /\r\n\r\n", 0, 400},
        /* Trailing space after the version. */
        {"GET / HTTP/1.1 \r\n\r\n", 0, 400},
        /* Version with junk appended. */
        {"GET / HTTP/1.1junk\r\n\r\n", 0, 400},
        /* Unknown major version. */
        {"GET / HTTP/2.1\r\n\r\n", 0, 505},
        {"GET / HTTP/9.9\r\n\r\n", 0, 505},
        /* Unknown minor version of major 1. */
        {"GET / HTTP/1.2\r\n\r\n", 0, 505},
        /* Bad method character. */
        {"G(T / HTTP/1.1\r\n\r\n", 0, 400},
        /* Control byte in the target. */
        {"GET /x\x01 HTTP/1.1\r\n\r\n", 0, 400},
        /* DEL in the target. */
        {"GET /x\x7f HTTP/1.1\r\n\r\n", 0, 400},
        /* Missing target. */
        {"GET  HTTP/1.1\r\n\r\n", 0, 400},
        /* Empty method. */
        {" / HTTP/1.1\r\n\r\n", 0, 400},
        /* Duplicate Host. */
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 0, 400},
        /* Empty Host. */
        {"GET / HTTP/1.1\r\nHost:\r\n\r\n", 0, 400},
        {"GET / HTTP/1.1\r\nHost: \r\n\r\n", 0, 400},
        /* Missing Host in HTTP/1.1. */
        {"GET / HTTP/1.1\r\nX-O: k\r\n\r\n", 0, 400},
        /* Space or tab before the colon. */
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 0, 400},
        {"GET / HTTP/1.1\r\nHost\t: x\r\n\r\n", 0, 400},
        /* Header line without a colon. */
        {"GET / HTTP/1.1\r\nHost: x\r\nnocolon\r\n\r\n", 0, 400},
        /* Invalid character in the header name. */
        {"GET / HTTP/1.1\r\nHost: x\r\nB@d: v\r\n\r\n", 0, 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n\x01" "Bad: v\r\n\r\n", 0, 400},
        /* DEL in the header value. */
        {"GET / HTTP/1.1\r\nHost: x\r\nX-V: a\x7f" "b\r\n\r\n", 0, 400},
        /* NUL in the header value (explicit length: string stops at NUL). */
        {"GET / HTTP/1.1\r\nHost: x\r\nX-V: a\0b\r\n\r\n", 37u, 400},
        /* Non-numeric Content-Length. */
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 12x\r\n\r\n", 0, 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n", 0, 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: \r\n\r\n", 0, 400},
        /* Conflicting duplicate Content-Length. */
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 0, 400},
        /* Transfer-Encoding combined with Content-Length. */
        {"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
         "Content-Length: 5\r\n\r\n",
         0, 400},
        /* Transfer-Encoding alone. */
        {"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", 0, 501},
        {"GET / HTTP/1.1\r\nHost: x\r\ntransfer-encoding: identity\r\n\r\n", 0, 501},
    };
    size_t i;

    dx_test_begin("http_parser/errors");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dx_http_parser_t p;
        size_t consumed = 0;
        size_t len = (cases[i].len != 0u) ? cases[i].len : strlen(cases[i].req);
        dx_parse_result_t r;

        dx_http_parser_init(&p, NULL);
        r = dx_http_parser_feed(&p, (const unsigned char *)cases[i].req, len, &consumed);
        if (r != DX_PARSE_ERROR) {
            fprintf(stderr, "  case %zu expected error, got %d: %.40s\n", i, (int)r,
                    cases[i].req);
        }
        CHECK_EQ(r, DX_PARSE_ERROR);
        CHECK_EQ(p.status_code, cases[i].status);
        CHECK_EQ(consumed, 0u);
    }

    /* Equal duplicate Content-Length values are accepted (RFC 9110 11.6). */
    {
        dx_http_parser_t p;
        size_t consumed = 0;

        CHECK_EQ(parse_whole(&p, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                                 "content-length: 5\r\n\r\n",
                             &consumed),
                 DX_PARSE_OK);
    }
}

/* Build "GET /a... HTTP/1.0\r\n\r\n" whose request line is exactly line_len.
 * HTTP/1.0 keeps the boundary case free of the Host requirement. */
static char *build_line_request(size_t line_len)
{
    char *buf;

    if (line_len < 14u) {
        return NULL;
    }
    buf = (char *)malloc(line_len + 8u);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, "GET ", 4u);
    memset(buf + 4u, 'a', line_len - 13u);  /* target fills the middle */
    memcpy(buf + line_len - 9u, " HTTP/1.0", 9u);
    memcpy(buf + line_len, "\r\n\r\n", 4u);
    return buf;
}

static void test_limit_errors(void)
{
    dx_http_limits_t limits;
    dx_http_parser_t p;
    size_t consumed = 0;
    char *big;

    dx_test_begin("http_parser/limits");
    dx_http_limits_default(&limits);

    /* Request line one byte past max_request_line -> 414. */
    big = build_line_request(limits.max_request_line + 1u);
    if (big == NULL) {
        CHECK(0);
        return;
    }
    dx_http_parser_init(&p, &limits);
    CHECK_EQ(dx_http_parser_feed(&p, (const unsigned char *)big, limits.max_request_line + 5u,
                                 &consumed),
             DX_PARSE_ERROR);
    CHECK_EQ(p.status_code, 414);
    free(big);

    /* Request line exactly at max_request_line -> OK. */
    big = build_line_request(limits.max_request_line);
    if (big == NULL) {
        CHECK(0);
        return;
    }
    dx_http_parser_init(&p, &limits);
    CHECK_EQ(dx_http_parser_feed(&p, (const unsigned char *)big, limits.max_request_line + 4u,
                                 &consumed),
             DX_PARSE_OK);
    free(big);

    /* Whole request one byte past max_header_size -> 431. */
    {
        size_t n = 33u; /* strlen("GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ") */
        size_t pad = limits.max_header_size;

        big = (char *)malloc(n + pad + 96u);
        if (big == NULL) {
            CHECK(0);
            return;
        }
        memcpy(big, "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ", n);
        memset(big + n, 'a', pad + 40u);
        dx_http_parser_init(&p, &limits);
        CHECK_EQ(dx_http_parser_feed(&p, (const unsigned char *)big, n + pad + 40u, &consumed),
                 DX_PARSE_ERROR);
        CHECK_EQ(p.status_code, 431);
        free(big);
    }

    /* max_headers = 10: ten headers are fine, eleven answer 431. */
    {
        char *req = (char *)calloc(1u, 1024u);
        size_t n;
        size_t count;

        if (req == NULL) {
            CHECK(0);
            return;
        }
        for (count = 9u; count <= 11u; count++) {
            memcpy(req, "GET / HTTP/1.1\r\nHost: x\r\n", 25u);
            n = 25u;
            for (size_t h = 0; h + 1u < count; h++) {
                n += (size_t)snprintf(req + n, 32u, "X-%02zu: v\r\n", h);
            }
            memcpy(req + n, "\r\n", 3u);
            n += 2u;
            limits.max_headers = 10u;
            dx_http_parser_init(&p, &limits);
            CHECK_EQ(dx_http_parser_feed(&p, (const unsigned char *)req, n, &consumed),
                     (count <= 10u) ? DX_PARSE_OK : DX_PARSE_ERROR);
            if (count == 11u) {
                CHECK_EQ(p.status_code, 431);
            }
        }
        free(req);
    }
}

static void test_reuse_and_guards(void)
{
    dx_http_parser_t p;
    size_t consumed = 0;

    dx_test_begin("http_parser/guards");

    /* A finished parser refuses more data until it is re-initialised. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", &consumed), DX_PARSE_OK);
    CHECK_EQ(dx_http_parser_feed(&p, (const unsigned char *)"GET / HTTP/1.1\r\n", 16u, &consumed),
             DX_PARSE_ERROR);
    CHECK_EQ(p.status_code, 500);

    /* NULL data or a NULL consumed sink must not crash. */
    dx_http_parser_init(&p, NULL);
    CHECK_EQ(dx_http_parser_feed(&p, NULL, 5u, &consumed), DX_PARSE_ERROR);
    CHECK_EQ(p.status_code, 500);

    /* After re-init the same struct parses a fresh request. */
    CHECK_EQ(parse_whole(&p, "GET / HTTP/1.0\r\n\r\n", &consumed), DX_PARSE_OK);
    CHECK_EQ(p.req.minor_version, 0);
}

int main(void)
{
    test_simple_get();
    test_minimal_requests();
    test_split_feeds();
    test_split_at_crlf_boundary();
    test_header_values();
    test_error_table();
    test_limit_errors();
    test_reuse_and_guards();
    return dx_test_summary();
}
