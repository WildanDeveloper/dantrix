/*
 * http_response.h - Response assembly and the v0.2 request dispatcher.
 *
 * Headers are built with bounded appends only (no sprintf into the output
 * buffer, no reflection of client input). Every response carries
 * X-Content-Type-Options: nosniff and Connection: close (keep-alive lands in
 * v0.4).
 */
#ifndef DX_HTTP_RESPONSE_H
#define DX_HTTP_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>

#include "http_parser.h"

/* Human readable reason phrase for the statuses Dantrix emits. Never NULL. */
const char *dx_http_status_reason(int status);

/*
 * Build a complete response into out.
 *
 *   status        HTTP status code
 *   extra_headers NULL or one or more complete "Name: value\r\n" lines
 *   content_type  NULL for no body (or use the error helper)
 *   body          may be NULL only when body_len is 0
 *   omit_body     true for HEAD: identical headers, no body bytes
 *
 * Returns the number of bytes written, or 0 when the buffer is too small
 * (callers size the buffer for the fixed bodies; 2 KiB is always enough).
 */
size_t dx_http_response_build(char *out, size_t cap, int status, const char *extra_headers,
                              const char *content_type, const char *body, size_t body_len,
                              bool omit_body);

/*
 * Build headers for a body that is streamed afterwards (sendfile). The body
 * is described by content_length only and nothing is appended after the
 * header terminator, which is what a file response needs. Returns the number
 * of bytes written, or 0 when the buffer is too small.
 */
size_t dx_http_response_headers(char *out, size_t cap, int status, const char *extra_headers,
                                const char *content_type, size_t content_length, bool omit_body);

/*
 * Build the fixed minimal error page for status. The body never reflects any
 * client input, which is what makes it safe against reflected XSS.
 * extra_headers may be NULL or complete "Name: value\r\n" lines (405 uses it
 * for Allow). Returns the number of bytes written, or 0 on overflow.
 */
size_t dx_http_response_error(char *out, size_t cap, int status, const char *extra_headers);

/*
 * Classify the request method. Returns:
 *   DX_DISPATCH_SERVE  GET/HEAD/OPTIONS: build a success response
 *   DX_DISPATCH_ERROR  status_out carries 405 (known, unsupported) with an
 *                      Allow header, or 501 (unknown method)
 */
typedef enum { DX_DISPATCH_SERVE = 0, DX_DISPATCH_ERROR = 1 } dx_dispatch_t;

dx_dispatch_t dx_http_dispatch_method(const dx_http_request_t *req, const unsigned char *base,
                                      int *status_out, const char **extra_headers);

#endif /* DX_HTTP_RESPONSE_H */
