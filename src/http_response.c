#include "http_response.h"

#include <string.h>

#include "dantrix.h"
#include "util.h"

/*
 * Bounded append buffer: an append past capacity flags an overflow instead of
 * writing out of bounds, so a mis-sized buffer degrades to an error return.
 */
typedef struct {
    char *data;
    size_t cap;
    size_t len;
    bool overflow;
} strbuf_t;

static void sb_init(strbuf_t *sb, char *data, size_t cap)
{
    sb->data = data;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = false;
}

static void sb_append(strbuf_t *sb, const char *s, size_t n)
{
    if (sb->overflow) {
        return;
    }
    if (n > sb->cap - sb->len) {
        sb->overflow = true;
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

static void sb_append_str(strbuf_t *sb, const char *s)
{
    sb_append(sb, s, strlen(s));
}

/* Append a non-negative number in decimal, without stdio formatting. */
static void sb_append_num(strbuf_t *sb, unsigned long long v)
{
    char tmp[24];
    size_t i = sizeof(tmp);

    if (sb->overflow) {
        return;
    }
    do {
        tmp[--i] = (char)('0' + (char)(v % 10ULL));
        v /= 10ULL;
    } while (v != 0ULL);
    sb_append(sb, tmp + i, sizeof(tmp) - i);
}

const char *dx_http_status_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 416:
        return "Range Not Satisfiable";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 505:
        return "HTTP Version Not Supported";
    default:
        break;
    }
    return "Unknown";
}

/*
 * Shared assembly. body_present tells whether the body bytes follow the
 * headers in this buffer; when false, content_length describes a body that is
 * streamed afterwards (sendfile) and nothing is appended.
 */
/*
 * Headers added to every response, configured once before the workers start (see
 * dx_http_response_set_extra_headers). Kept here rather than threaded through
 * every call site so a new response path cannot forget them: they are emitted in
 * response_assemble(), which every path goes through.
 */
static char (*g_add_header)[DX_CFG_STR_MAX];
static size_t g_add_header_count;

void dx_http_response_set_extra_headers(char (*headers)[DX_CFG_STR_MAX], size_t count)
{
    g_add_header = headers;
    g_add_header_count = (headers != NULL) ? count : 0u;
}

static size_t response_assemble(char *out, size_t cap, int status, const char *extra_headers,
                                const char *content_type, const char *body, size_t content_length,
                                bool omit_body, bool body_present, const char *connection,
                                bool server_tokens)
{
    strbuf_t sb;
    char date[40];

    if (out == NULL || cap == 0) {
        return 0;
    }
    if (body == NULL && body_present && content_length != 0) {
        return 0;
    }
    sb_init(&sb, out, cap);

    /* Status line. */
    sb_append_str(&sb, "HTTP/1.1 ");
    sb_append_num(&sb, (unsigned long long)status);
    sb_append_str(&sb, " ");
    sb_append_str(&sb, dx_http_status_reason(status));
    sb_append_str(&sb, "\r\n");

    /* Fixed server headers. The token line can be switched off with
     * --server-tokens off; it used to be emitted unconditionally, so the flag
     * was parsed and then silently ignored. */
    if (server_tokens) {
        sb_append_str(&sb, "Server: " DX_PROGRAM_NAME "/" DX_VERSION_STRING "\r\n");
    }

    if (dx_http_date_cached(date, sizeof(date)) > 0) {
        sb_append_str(&sb, "Date: ");
        sb_append_str(&sb, date);
        sb_append_str(&sb, "\r\n");
    }

    if (content_type != NULL) {
        sb_append_str(&sb, "Content-Type: ");
        sb_append_str(&sb, content_type);
        sb_append_str(&sb, "\r\n");
    }

    /*
     * Content-Length always describes the body the client would receive, so a
     * HEAD response carries the same length as its GET twin (RFC 9110 9.3.2).
     *
     * 204 and 304 must not carry one at all: neither has a body, and a length
     * on a 304 that does not match the 200 it answers makes clients truncate
     * or wait. "Content-Length: 0" on a 304 told the client the cached
     * representation is empty.
     */
    if (status != 204 && status != 304) {
        sb_append_str(&sb, "Content-Length: ");
        sb_append_num(&sb, (unsigned long long)content_length);
        sb_append_str(&sb, "\r\n");
    }

    if (connection != NULL && connection[0] != '\0') {
        sb_append_str(&sb, "Connection: ");
        sb_append_str(&sb, connection);
        sb_append_str(&sb, "\r\n");
    }
    sb_append_str(&sb, "X-Content-Type-Options: nosniff\r\n");

    if (extra_headers != NULL) {
        sb_append_str(&sb, extra_headers);
    }
    /* The operator's configured headers, on every response. Validated at
     * start-up, so they are emitted verbatim; a CR or LF here would split the
     * response, which is why the configuration refuses one. */
    for (size_t i = 0; i < g_add_header_count; i++) {
        const char *h = g_add_header[i];

        if (h != NULL && h[0] != '\0') {
            sb_append_str(&sb, h);
            sb_append_str(&sb, "\r\n");
        }
    }
    sb_append_str(&sb, "\r\n");

    if (body_present && !omit_body && body != NULL && content_length > 0) {
        sb_append(&sb, body, content_length);
    }

    if (sb.overflow) {
        return 0;
    }
    return sb.len;
}

size_t dx_http_response_build(char *out, size_t cap, int status, const char *extra_headers,
                              const char *content_type, const char *body, size_t body_len,
                              bool omit_body)
{
    return response_assemble(out, cap, status, extra_headers, content_type, body, body_len,
                             omit_body, true, "close", true);
}

size_t dx_http_response_build_ex(char *out, size_t cap, int status, const char *extra_headers,
                                 const char *content_type, const char *body, size_t body_len,
                                 bool omit_body, const char *connection, bool server_tokens)
{
    return response_assemble(out, cap, status, extra_headers, content_type, body, body_len,
                             omit_body, true, connection, server_tokens);
}

size_t dx_http_response_headers(char *out, size_t cap, int status, const char *extra_headers,
                                const char *content_type, size_t content_length, bool omit_body,
                                const char *connection, bool server_tokens)
{
    /* No body bytes follow: Content-Length describes the streamed file. */
    return response_assemble(out, cap, status, extra_headers, content_type, NULL, content_length,
                             omit_body, false, connection, server_tokens);
}

size_t dx_http_response_error(char *out, size_t cap, int status, const char *extra_headers)
{
    return dx_http_response_error_ex(out, cap, status, extra_headers, "close", true, false);
}

size_t dx_http_response_error_ex(char *out, size_t cap, int status, const char *extra_headers,
                                 const char *connection, bool server_tokens, bool omit_body)
{
    /*
     * Fixed body: the status and reason are server controlled, so no client
     * input is ever reflected (reflected XSS defence).
     */
    char body[256];
    strbuf_t b;
    const char *reason = dx_http_status_reason(status);

    sb_init(&b, body, sizeof(body));
    sb_append_str(&b, "<html><head><title>");
    sb_append_num(&b, (unsigned long long)status);
    sb_append_str(&b, " ");
    sb_append_str(&b, reason);
    sb_append_str(&b, "</title></head><body><h1>");
    sb_append_num(&b, (unsigned long long)status);
    sb_append_str(&b, " ");
    sb_append_str(&b, reason);
    sb_append_str(&b, "</h1><hr><address>" DX_PROGRAM_NAME "</address></body></html>");
    if (b.overflow) {
        return 0;
    }

    return response_assemble(out, cap, status, extra_headers, "text/html; charset=utf-8", body,
                             b.len, omit_body, true, connection, server_tokens);
}

dx_dispatch_t dx_http_dispatch_method(const dx_http_request_t *req, const unsigned char *base,
                                      int *status_out, const char **extra_headers)
{
    size_t mlen;
    const char *method;

    if (req == NULL || base == NULL) {
        if (status_out != NULL) {
            *status_out = 400;
        }
        return DX_DISPATCH_ERROR;
    }
    mlen = req->method_len;
    method = (const char *)base + req->method_off;

    /* Supported: GET, HEAD, OPTIONS. Method names are case sensitive per RFC
     * 9110 9.1, but being liberal here only accepts what strict servers
     * reject; keep the comparison case sensitive and simple. */
#define METHOD_IS(s) (mlen == sizeof(s) - 1u && memcmp(method, s, sizeof(s) - 1u) == 0)

    if (METHOD_IS("GET") || METHOD_IS("HEAD") || METHOD_IS("OPTIONS")) {
        return DX_DISPATCH_SERVE;
    }

    /* Known but unsupported methods answer 405 with Allow; unknown ones 501. */
    if (METHOD_IS("POST") || METHOD_IS("PUT") || METHOD_IS("DELETE") || METHOD_IS("PATCH") ||
        METHOD_IS("TRACE") || METHOD_IS("CONNECT")) {
        if (status_out != NULL) {
            *status_out = 405;
        }
        if (extra_headers != NULL) {
            *extra_headers = "Allow: GET, HEAD, OPTIONS\r\n";
        }
        return DX_DISPATCH_ERROR;
    }
#undef METHOD_IS

    if (status_out != NULL) {
        *status_out = 501;
    }
    return DX_DISPATCH_ERROR;
}
