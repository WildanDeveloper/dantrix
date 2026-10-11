#include "http_parser.h"

#include <strings.h>
#include <string.h>

#include "util.h"

void dx_http_limits_default(dx_http_limits_t *limits)
{
    if (limits == NULL) {
        return;
    }
    limits->max_request_line = 8u * 1024u;
    limits->max_header_size = 16u * 1024u;
    limits->max_headers = 100u;
}

void dx_http_parser_init(dx_http_parser_t *p, const dx_http_limits_t *limits)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->state = DX_PS_REQUEST_LINE;
    if (limits != NULL) {
        p->limits = *limits;
    } else {
        dx_http_limits_default(&p->limits);
    }
}

/* True for RFC 9110 tchar: allowed in tokens (method, header names). */
static bool is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return (c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
            c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' ||
            c == '~');
}

/* Optional whitespace in header values. */
static bool is_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

/* Record a protocol error with the status code to answer with. */
static dx_parse_result_t parser_fail(dx_http_parser_t *p, int status)
{
    p->state = DX_PS_DONE;
    p->status_code = status;
    return DX_PARSE_ERROR;
}

/*
 * Parse the completed request line [line_start, line_end). Offsets are stored
 * relative to the fed window. Every deviation from
 * "METHOD SP target SP version" with single spaces is a 400.
 * Returns 0 on success, -1 on error (status_code set).
 */
static int parse_request_line(dx_http_parser_t *p, const unsigned char *data, size_t line_end)
{
    size_t i = p->line_start;
    size_t method_start;
    size_t method_len;
    size_t target_start;
    size_t target_len;
    size_t version_start;
    size_t version_len;

    /* Method: tchars until the first SP. */
    method_start = i;
    while (i < line_end && data[i] != ' ') {
        if (!is_tchar(data[i])) {
            p->status_code = 400;
            return -1;
        }
        i++;
    }
    if (i == line_end || i == method_start) {
        /* No SP at all, or an empty method: not a request line. */
        p->status_code = 400;
        return -1;
    }
    method_len = i - method_start;

    /* Target: visible bytes until the next SP. */
    target_start = ++i;
    while (i < line_end && data[i] != ' ') {
        unsigned char c = data[i];

        /* Control characters and DEL are never valid in a request target. */
        if (c < 0x21u || c == 0x7fu) {
            p->status_code = 400;
            return -1;
        }
        i++;
    }
    if (i == line_end || i == target_start) {
        p->status_code = 400;
        return -1;
    }
    target_len = i - target_start;

    /* Version: exactly one SP, then "HTTP/1.x" to the end of the line. */
    version_start = ++i;
    if (line_end <= version_start) {
        p->status_code = 400;
        return -1;
    }
    version_len = line_end - version_start;

    if (version_len != 8u || memcmp(data + version_start, "HTTP/", 5u) != 0 ||
        data[version_start + 6u] != '.') {
        p->status_code = 400;
        return -1;
    }
    if (data[version_start + 5u] != '1') {
        /* Only major version 1 is implemented: RFC 9110 says answer 505. */
        p->status_code = 505;
        return -1;
    }
    if (data[version_start + 7u] == '0') {
        p->req.minor_version = 0;
    } else if (data[version_start + 7u] == '1') {
        p->req.minor_version = 1;
    } else {
        /* Unknown minor of major 1 has never been defined: answer 505. */
        p->status_code = 505;
        return -1;
    }

    p->req.method_off = method_start;
    p->req.method_len = method_len;
    p->req.target_off = target_start;
    p->req.target_len = target_len;
    return 0;
}

/*
 * Parse one completed header line [line_start, line_end). Returns:
 *   0  header stored
 *   1  line was empty (end of the header section)
 *  -1  error (status_code set)
 */
static int parse_header_line(dx_http_parser_t *p, const unsigned char *data, size_t line_end)
{
    size_t i = p->line_start;
    size_t name_start = i;
    size_t name_len;
    size_t value_start;
    size_t value_end;
    dx_http_header_t *h;

    if (i == line_end) {
        return 1; /* empty line: end of headers */
    }

    /* Name: tchars only. Whitespace before the colon is a protocol error. */
    while (i < line_end && data[i] != ':') {
        if (!is_tchar(data[i])) {
            p->status_code = 400;
            return -1;
        }
        i++;
    }
    if (i == line_end || i == name_start) {
        /* No colon, or an empty name. */
        p->status_code = 400;
        return -1;
    }
    name_len = i - name_start;

    /* Value: skip leading OWS, strip trailing OWS, reject control bytes. */
    value_start = ++i;
    while (i < line_end && is_ows(data[i])) {
        i++;
        value_start++;
    }
    value_end = line_end;
    while (value_end > value_start && is_ows(data[value_end - 1u])) {
        value_end--;
    }
    for (i = value_start; i < value_end; i++) {
        unsigned char c = data[i];

        if ((c < 0x20u && c != '\t') || c == 0x7fu) {
            p->status_code = 400;
            return -1;
        }
    }

    if (p->req.num_headers >= DX_MAX_HEADERS_LIMIT || p->req.num_headers >= p->limits.max_headers) {
        p->status_code = 431;
        return -1;
    }
    h = &p->req.headers[p->req.num_headers++];
    h->name_off = name_start;
    h->name_len = name_len;
    h->value_off = value_start;
    h->value_len = value_end - value_start;

    /* Track the headers whose presence changes protocol decisions. */
    if (name_len == 4u && strncasecmp((const char *)data + name_start, "Host", 4u) == 0) {
        if (p->host_seen) {
            p->status_code = 400; /* duplicate Host */
            return -1;
        }
        p->host_seen = true;
        if (h->value_len == 0) {
            p->status_code = 400; /* empty Host */
            return -1;
        }
    } else if (name_len == 14u &&
               strncasecmp((const char *)data + name_start, "Content-Length", 14u) == 0) {
        unsigned long long v = 0;
        bool ok = dx_parse_ull((const char *)data + h->value_off, h->value_len, &v);

        if (!ok) {
            p->status_code = 400; /* non-numeric or overflowing Content-Length */
            return -1;
        }
        if (p->content_length_seen && p->content_length != v) {
            p->status_code = 400; /* conflicting duplicates */
            return -1;
        }
        p->content_length_seen = true;
        p->content_length = (size_t)v;
    } else if (name_len == 17u &&
               strncasecmp((const char *)data + name_start, "Transfer-Encoding", 17u) == 0) {
        p->transfer_encoding_seen = true;
    }
    return 0;
}

dx_parse_result_t dx_http_parser_feed(dx_http_parser_t *p, const unsigned char *data, size_t len,
                                      size_t *consumed)
{
    size_t i;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (p == NULL) {
        return DX_PARSE_ERROR; /* nowhere to record the status code */
    }
    if (data == NULL) {
        return parser_fail(p, 500);
    }
    if (p->state == DX_PS_DONE) {
        /* A finished parser must be re-initialised before reuse. */
        return parser_fail(p, 500);
    }

    for (i = p->scan; i < len; i++) {
        unsigned char c = data[i];

        p->request_size++;
        if (p->request_size > p->limits.max_header_size) {
            return parser_fail(p, 431);
        }
        /*
         * The request line limit covers the line content, not the CRLF. CR
         * bytes may still turn out to be the line terminator, so they count
         * as content only tentatively; the LF itself never does.
         */
        if (p->state == DX_PS_REQUEST_LINE && c != '\n') {
            size_t line_len_so_far = (c == '\r') ? (i - p->line_start) : (i - p->line_start + 1u);

            if (line_len_so_far > p->limits.max_request_line) {
                return parser_fail(p, 414);
            }
        }

        if (c == '\n') {
            size_t line_end;

            if (!p->prev_cr) {
                return parser_fail(p, 400); /* bare LF */
            }
            /* The line content is [line_start, i - 1): CRLF excluded. */
            line_end = i - 1u;

            if (p->state == DX_PS_REQUEST_LINE) {
                if (parse_request_line(p, data, line_end) != 0) {
                    return parser_fail(p, p->status_code);
                }
                p->state = DX_PS_HEADERS;
            } else {
                int hr = parse_header_line(p, data, line_end);

                if (hr < 0) {
                    return parser_fail(p, p->status_code);
                }
                if (hr == 1) {
                    /* End of headers: run the cross-header validations. */
                    if (p->transfer_encoding_seen && p->content_length_seen) {
                        return parser_fail(p, 400);
                    }
                    if (p->transfer_encoding_seen) {
                        return parser_fail(p, 501);
                    }
                    if (p->req.minor_version == 1 && !p->host_seen) {
                        return parser_fail(p, 400);
                    }
                    p->req.has_content_length = p->content_length_seen;
                    p->req.content_length = (uint64_t)p->content_length;
                    p->state = DX_PS_DONE;
                    if (consumed != NULL) {
                        *consumed = i + 1u;
                    }
                    return DX_PARSE_OK;
                }
            }
            p->line_start = i + 1u;
            p->prev_cr = false;
            continue;
        }

        p->prev_cr = (c == '\r');
    }

    p->scan = len;
    return DX_PARSE_NEED_DATA;
}

bool dx_http_request_find_header(const dx_http_request_t *req, const unsigned char *base,
                                 const char *name, size_t *value_off, size_t *value_len)
{
    size_t name_len;

    if (req == NULL || base == NULL || name == NULL) {
        return false;
    }
    name_len = strlen(name);
    for (size_t k = 0; k < req->num_headers; k++) {
        const dx_http_header_t *h = &req->headers[k];

        if (h->name_len == name_len &&
            strncasecmp((const char *)base + h->name_off, name, name_len) == 0) {
            if (value_off != NULL) {
                *value_off = h->value_off;
            }
            if (value_len != NULL) {
                *value_len = h->value_len;
            }
            return true;
        }
    }
    return false;
}
