/*
 * http_parser.h - Incremental, strict HTTP/1.x request line and header parser.
 *
 * Design rules:
 *  - Incremental: the parser keeps its scan position across feeds, so a
 *    request may arrive in any number of pieces (this is proven by the split
 *    tests in tests/unit/test_http_parser.c).
 *  - Zero copy: the parsed request stores offsets relative to the start of
 *    the fed window, never pointers. The caller resolves them against its own
 *    buffer, which may move between feeds.
 *  - Strict: anything ambiguous is rejected with a status code and the
 *    connection must be closed afterwards (see README "Strict parser rules").
 */
#ifndef DX_HTTP_PARSER_H
#define DX_HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hard compile-time ceiling for the number of headers per request. */
#define DX_MAX_HEADERS_LIMIT 256u

/* Server-side limits applied while parsing (defaults mirror the README). */
typedef struct {
    size_t max_request_line; /* bytes; exceeded -> 414 */
    size_t max_header_size;  /* bytes for the whole request; exceeded -> 431 */
    size_t max_headers;      /* header count; exceeded -> 431 */
} dx_http_limits_t;

/* Default limits, used when the caller does not override them. */
void dx_http_limits_default(dx_http_limits_t *limits);

typedef enum {
    DX_PARSE_NEED_DATA = 0, /* incomplete request: feed more bytes */
    DX_PARSE_OK,            /* complete request: req is filled, *consumed set */
    DX_PARSE_ERROR          /* protocol error: status_code set, must close */
} dx_parse_result_t;

typedef struct {
    size_t name_off; /* offset of the header name in the fed window */
    size_t name_len;
    size_t value_off; /* offset of the value, OWS already stripped */
    size_t value_len;
} dx_http_header_t;

typedef struct {
    size_t method_off;
    size_t method_len;
    size_t target_off;
    size_t target_len;
    int minor_version; /* 0 for HTTP/1.0, 1 for HTTP/1.1 */
    /*
     * The header table is NOT embedded: an inline array of the compile-time
     * maximum made every connection carry 8 KiB (256 x 32 bytes) even while it
     * sat idle between keep-alive requests, which is what pushed the per
     * connection cost to ~9 KiB against the documented 1 KiB target. The
     * storage is now handed in by the caller (see dx_http_parser_init) and can
     * be sized to the configured max_headers and returned when the connection
     * goes idle, so memory follows active work instead of open sockets.
     */
    dx_http_header_t *headers;
    size_t num_headers;
    /* Parsed Content-Length of the request body, for the drain path. */
    uint64_t content_length;
    bool has_content_length;
} dx_http_request_t;

typedef enum { DX_PS_REQUEST_LINE = 0, DX_PS_HEADERS, DX_PS_DONE } dx_parse_state_t;

typedef struct {
    dx_parse_state_t state;
    dx_http_limits_t limits;
    dx_http_request_t req;

    /* Scan position and request size, both relative to the fed window. */
    size_t scan;
    size_t request_size;
    size_t line_start; /* offset where the current line begins */

    /* Cross-feed byte context. */
    bool prev_cr; /* last examined byte was CR */

    /* Request line assembly. */

    /* Header accumulation. */
    size_t header_cap; /* entries available in req.headers; 0 = none yet */

    /* Validation state. */
    bool host_seen;
    bool content_length_seen;
    size_t content_length;
    bool transfer_encoding_seen;

    /* On DX_PARSE_ERROR: the status code to answer with. */
    int status_code;
} dx_http_parser_t;

/*
 * Reset the parser for a new request on the same connection. limits is copied;
 * pass NULL to use the built-in defaults.
 *
 * headers/header_cap describe a caller-owned table. The parser stores the
 * pointer and never allocates: a request with more headers than header_cap is
 * answered with 431 instead of writing past the table. NULL/0 is allowed and
 * makes every header-bearing request 431, which keeps the request-line-only
 * unit tests working.
 */
void dx_http_parser_init_ex(dx_http_parser_t *p, const dx_http_limits_t *limits,
                            dx_http_header_t *headers, size_t header_cap);

/* As above with no header storage: any header-bearing request becomes 431. */
void dx_http_parser_init(dx_http_parser_t *p, const dx_http_limits_t *limits);

/*
 * Feed the next contiguous piece of the request. data must start exactly at
 * the first byte of the request and must stay valid (with the same base) until
 * the request completes: every offset in req is relative to it.
 *
 * On DX_PARSE_OK, *consumed receives the number of bytes that belong to this
 * request, including the final empty-line CRLF. On the other results,
 * *consumed is set to 0 and the caller must feed more data later.
 */
dx_parse_result_t dx_http_parser_feed(dx_http_parser_t *p, const unsigned char *data, size_t len,
                                      size_t *consumed);

/*
 * Case-insensitive header lookup in a parsed request. Returns the value
 * offsets, or false when the header is absent.
 */
bool dx_http_request_find_header(const dx_http_request_t *req, const unsigned char *base,
                                 const char *name, size_t *value_off, size_t *value_len);

#endif /* DX_HTTP_PARSER_H */
