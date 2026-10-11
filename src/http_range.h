/*
 * http_range.h - Range parsing, ETag formatting and conditional request
 * evaluation. Everything here is pure logic with no I/O, so it is directly
 * unit-testable.
 *
 * README rules implemented:
 *   - only a single range is served; multi-range is ignored (full 200), which
 *     closes the overlapping-range attack class
 *   - a range beyond the file size is 416, invalid syntax is ignored
 *   - If-None-Match uses weak comparison, supports "*" and lists, and takes
 *     precedence over If-Modified-Since
 *   - If-Range only accepts strong validators
 */
#ifndef DX_HTTP_RANGE_H
#define DX_HTTP_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DX_RANGE_NONE = 0,     /* no usable Range: answer with a full 200 */
    DX_RANGE_OK,           /* single range parsed, see dx_range_t */
    DX_RANGE_UNSATISFIABLE /* start beyond the size: answer 416 */
} dx_range_result_t;

typedef struct {
    uint64_t start;  /* inclusive */
    uint64_t end;    /* inclusive */
    uint64_t length; /* end - start + 1 */
} dx_range_t;

/*
 * Parse a Range header value against a representation of size bytes.
 * Multi-range values and invalid syntax yield DX_RANGE_NONE.
 */
dx_range_result_t dx_http_range_parse(const char *value, size_t len, uint64_t size,
                                      dx_range_t *out);

/*
 * Write an ETag in the nginx style "\"<mtime-hex>-<size-hex>\"" into buf.
 * Returns the number of bytes written excluding the terminator, or 0 when the
 * buffer is too small.
 */
size_t dx_http_etag_format(char *buf, size_t cap, long long mtime_sec, uint64_t size);

/* Weak comparison (RFC 9110 8.8.3.2): "*" or any list member equal to etag. */
bool dx_http_etag_matches(const char *value, size_t len, const char *etag);

/* Strictly parse an IMF-fixdate ("Sun, 11 Oct 2026 03:15:30 GMT"). */
bool dx_http_parse_date(const char *s, size_t len, long long *out_sec);

/* If-Modified-Since: true when the resource is not modified after the date. */
bool dx_http_modified_since(const char *value, size_t len, long long mtime_sec);

/* If-Range: strong validator only (exact ETag or exact Last-Modified date). */
bool dx_http_if_range_match(const char *value, size_t len, const char *etag, long long mtime_sec);

#endif /* DX_HTTP_RANGE_H */