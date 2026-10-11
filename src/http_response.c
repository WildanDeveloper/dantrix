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
    case 417:
        return "Expectation Failed";
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

size_t dx_http_response_build(char *out, size_t cap, int status, const char *extra_headers,
                              const char *content_type, const char *body, size_t body_len,
                              bool omit_body)
{
    strbuf_t sb;
    char date[40];

    if (out == NULL || cap == 0) {
        return 0;
    }
    if (body == NULL && body_len != 0) {
        return 0;
    }
    sb_init(&sb, out, cap);

    /* Status line. */
    sb_append_str(&sb, "HTTP/1.1 ");
    sb_append_num(&sb, (unsigned long long)status);
    sb_append_str(&sb, " ");
    sb_append_str(&sb, dx_http_status_reason(status));
    sb_append_str(&sb, "\r\n");

    /* Fixed server headers. */
    sb_append_str(&sb, "Server: " DX_PROGRAM_NAME "/" DX_VERSION_STRING "\r\n");

    if (dx_http_date(date, sizeof(date), -1) > 0) {
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
     */
    sb_append_str(&sb, "Content-Length: ");
    sb_append_num(&sb, (unsigned long long)body_len);
    sb_append_str(&sb, "\r\n");

    sb_append_str(&sb, "Connection: close\r\n");
    sb_append_str(&sb, "X-Content-Type-Options: nosniff\r\n");

    if (extra_headers != NULL) {
        sb_append_str(&sb, extra_headers);
    }
    sb_append_str(&sb, "\r\n");

    if (!omit_body && body != NULL && body_len > 0) {
        sb_append(&sb, body, body_len);
    }

    if (sb.overflow) {
        return 0;
    }
    return sb.len;
}

size_t dx_http_response_error(char *out, size_t cap, int status, const char *extra_headers)
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

    return dx_http_response_build(out, cap, status, extra_headers, "text/html; charset=utf-8", body,
                                  b.len, false);
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
