/*
 * Unit tests for src/http_response.c: bounded assembly, HEAD semantics,
 * fixed error pages and the method dispatcher.
 */
#include "testing.h"

#include "http_response.h"

#include <string.h>

static void test_build_200(void)
{
    char out[2048];
    size_t n;
    static const char body[] = "hello";

    dx_test_begin("http_response/build_200");

    n = dx_http_response_build(out, sizeof(out), 200, NULL, "text/plain", body, sizeof(body) - 1u,
                               false);
    CHECK(n > 0);
    CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17u) == 0);
    CHECK(strstr(out, "\r\nServer: Dantrix/") != NULL);
    CHECK(strstr(out, "Content-Type: text/plain\r\n") != NULL);
    CHECK(strstr(out, "Content-Length: 5\r\n") != NULL);
    CHECK(strstr(out, "Connection: close\r\n") != NULL);
    CHECK(strstr(out, "X-Content-Type-Options: nosniff\r\n") != NULL);
    CHECK(strstr(out, "Date: ") != NULL);
    /* The body sits right after the terminating empty line. */
    CHECK(strstr(out, "\r\n\r\nhello") != NULL);
    /* Exactly one double CRLF (end of headers). */
    {
        size_t hits = 0;
        const char *scan = out;

        while ((scan = strstr(scan, "\r\n\r\n")) != NULL) {
            hits++;
            scan += 2u;
        }
        CHECK_EQ(hits, 1u);
    }
    CHECK(strncmp(out + n - 5u, "hello", 5u) == 0);
}

static void test_head_semantics(void)
{
    char out[2048];
    size_t n;
    static const char body[] = "12345678";

    dx_test_begin("http_response/head");

    n = dx_http_response_build(out, sizeof(out), 200, NULL, "text/plain", body, 8u, true);
    CHECK(n > 0);
    /* Identical headers, including Content-Length, but no body bytes. */
    CHECK(strstr(out, "Content-Length: 8\r\n") != NULL);
    CHECK(strstr(out, "Content-Type: text/plain\r\n") != NULL);
    CHECK(strncmp(out + n - 4u, "\r\n\r\n", 4u) == 0);
}

static void test_overflow_returns_zero(void)
{
    char small[64];
    char tiny[8];

    dx_test_begin("http_response/overflow");

    /* A too small buffer degrades to 0 instead of writing out of bounds. */
    CHECK_EQ(dx_http_response_build(small, sizeof(small), 200, NULL, "text/plain", "x", 1u, false),
             0u);
    CHECK_EQ(dx_http_response_build(tiny, sizeof(tiny), 200, NULL, NULL, NULL, 0u, false), 0u);
    CHECK_EQ(dx_http_response_error(small, sizeof(small), 404, NULL), 0u);
}

static void test_error_pages(void)
{
    char out[2048];
    size_t n;

    dx_test_begin("http_response/errors");

    n = dx_http_response_error(out, sizeof(out), 400, NULL);
    CHECK(n > 0);
    CHECK(strncmp(out, "HTTP/1.1 400 Bad Request\r\n", 26u) == 0);
    CHECK(strstr(out, "<h1>400 Bad Request</h1>") != NULL);
    CHECK(strstr(out, "text/html; charset=utf-8") != NULL);

    /* 405 carries the Allow header. */
    n = dx_http_response_error(out, sizeof(out), 405, "Allow: GET, HEAD, OPTIONS\r\n");
    CHECK(strstr(out, "Allow: GET, HEAD, OPTIONS\r\n") != NULL);
    CHECK(strncmp(out, "HTTP/1.1 405 Method Not Allowed\r\n", 33u) == 0);

    /* 505 and 431 and 501 all have their reason phrases. */
    n = dx_http_response_error(out, sizeof(out), 505, NULL);
    CHECK(strncmp(out, "HTTP/1.1 505 HTTP Version Not Supported\r\n", 41u) == 0);
    n = dx_http_response_error(out, sizeof(out), 431, NULL);
    CHECK(strstr(out, "Request Header Fields Too Large") != NULL);
    n = dx_http_response_error(out, sizeof(out), 501, NULL);
    CHECK(strstr(out, "501 Not Implemented") != NULL);

    /* Unknown codes degrade to a generic reason, never NULL. */
    CHECK(strcmp(dx_http_status_reason(599), "Unknown") == 0);
    CHECK(strcmp(dx_http_status_reason(200), "OK") == 0);
}

static void test_options_and_zero_body(void)
{
    char out[2048];
    size_t n;

    dx_test_begin("http_response/options");

    n = dx_http_response_build(out, sizeof(out), 200, "Allow: GET, HEAD, OPTIONS\r\n", NULL, NULL,
                               0u, false);
    CHECK(n > 0);
    CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);
    CHECK(strstr(out, "Allow: GET, HEAD, OPTIONS\r\n") != NULL);
    CHECK(strstr(out, "Content-Type:") == NULL);
    /* No body: the response ends with the header terminator. */
    CHECK(strncmp(out + n - 4u, "\r\n\r\n", 4u) == 0);
}

static void test_dispatch(void)
{
    dx_http_parser_t p;
    dx_http_request_t *req = &p.req;
    static unsigned char base[64];
    int status = 0;
    const char *extra = NULL;

    dx_test_begin("http_response/dispatch");

    memcpy(base, "GET / HTTP/1.1", 14u);
    dx_http_parser_init(&p, NULL);
    req->method_off = 0u;
    req->method_len = 3u;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_SERVE);

    memcpy(base, "HEAD / HTTP/1.1", 15u);
    req->method_len = 4u;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_SERVE);

    memcpy(base, "OPTIONS * HTTP/1.1", 18u);
    req->method_len = 7u;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_SERVE);

    /* Methods are case sensitive: lowercase is an unknown method. */
    memcpy(base, "get / HTTP/1.1", 14u);
    req->method_len = 3u;
    status = 0;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 501);

    /* Known but unsupported: 405 plus Allow. */
    memcpy(base, "POST / HTTP/1.1", 15u);
    req->method_len = 4u;
    status = 0;
    extra = NULL;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 405);
    CHECK(extra != NULL && strstr(extra, "Allow:") == extra);

    memcpy(base, "DELETE / HTTP/1.1", 17u);
    req->method_len = 6u;
    status = 0;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 405);

    memcpy(base, "TRACE / HTTP/1.1", 16u);
    req->method_len = 5u;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 405);

    /* Unknown method: 501 without Allow. */
    memcpy(base, "BREW / HTTP/1.1", 15u);
    req->method_len = 4u;
    status = 0;
    extra = NULL;
    CHECK_EQ(dx_http_dispatch_method(req, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 501);
    CHECK(extra == NULL);

    /* NULL request answers 400 rather than crashing. */
    status = 0;
    CHECK_EQ(dx_http_dispatch_method(NULL, base, &status, &extra), DX_DISPATCH_ERROR);
    CHECK_EQ(status, 400);
}

int main(void)
{
    test_build_200();
    test_head_semantics();
    test_overflow_returns_zero();
    test_error_pages();
    test_options_and_zero_body();
    test_dispatch();
    return dx_test_summary();
}
