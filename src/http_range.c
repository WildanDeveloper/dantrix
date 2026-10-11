#include "http_range.h"

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <time.h>

static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* Trim optional whitespace from both ends of [s, s+len). */
static void trim(const char **s, size_t *len)
{
    while (*len > 0 && is_ows((*s)[0])) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && is_ows((*s)[*len - 1u])) {
        (*len)--;
    }
}

/*
 * Parse a decimal run into out. Returns the number of digits consumed, or 0
 * when there is no digit. Values above UINT64_MAX saturate: callers treat that
 * as "beyond the end of any file".
 */
static size_t parse_digits(const char *s, size_t len, uint64_t *out)
{
    uint64_t value = 0;
    size_t i = 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (value > (UINT64_MAX - 9u) / 10u) {
            value = UINT64_MAX;
        } else {
            value = value * 10u + (uint64_t)(s[i] - '0');
        }
        i++;
    }
    *out = value;
    return i;
}

static size_t write_hex(char *buf, size_t cap, uint64_t value)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    size_t n = 0;
    size_t i;

    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0) {
        tmp[n++] = hex[value & 0xfu];
        value >>= 4;
    }
    if (n + 1u > cap) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1u - i];
    }
    buf[n] = '\0';
    return n;
}

size_t dx_http_etag_format(char *buf, size_t cap, long long mtime_sec, uint64_t size)
{
    char body[40];
    size_t n;
    size_t m;

    if (buf == NULL || cap == 0) {
        return 0;
    }
    /* Timestamps are non-negative for any real file; a negative one is a bug
     * upstream, so clamp instead of rendering a minus sign into a header. */
    if (mtime_sec < 0) {
        mtime_sec = 0;
    }
    n = write_hex(body, sizeof(body), (uint64_t)mtime_sec);
    m = write_hex(body + n + 1u, sizeof(body) - n - 1u, size);
    if (n == 0 || m == 0) {
        return 0;
    }
    body[n] = '-';
    {
        int written = snprintf(buf, cap, "\"%s\"", body);

        if (written < 0 || (size_t)written >= cap) {
            buf[0] = '\0';
            return 0;
        }
        return (size_t)written;
    }
}

/*
 * Weak comparison (RFC 9110 8.8.3.2): the opaque tags must match character by
 * character regardless of either or both being weak, so a "W/" prefix on
 * either side is skipped.
 */
static bool tag_equals(const char *item, size_t len, const char *etag)
{
    if (len >= 2u && item[0] == 'W' && item[1] == '/') {
        item += 2;
        len -= 2u;
    }
    if (etag[0] == 'W' && etag[1] == '/') {
        etag += 2;
    }
    if (len != strlen(etag)) {
        return false;
    }
    return strncmp(item, etag, len) == 0;
}

bool dx_http_etag_matches(const char *value, size_t len, const char *etag)
{
    size_t pos = 0;

    if (value == NULL || etag == NULL) {
        return false;
    }
    trim(&value, &len);
    if (len == 0) {
        return false;
    }
    if (len == 1u && value[0] == '*') {
        return true;
    }
    while (pos < len) {
        size_t start = pos;

        while (pos < len && value[pos] != ',') {
            pos++;
        }
        {
            const char *item = value + start;
            size_t item_len = pos - start;

            trim(&item, &item_len);
            if (tag_equals(item, item_len, etag)) {
                return true;
            }
        }
        pos++; /* skip the comma */
    }
    return false;
}

static bool month_from_name(const char *s, size_t len, int *out)
{
    size_t i;

    for (i = 0; i < 12u; i++) {
        if (len == 3u && strncmp(s, month_names[i], 3u) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

/*
 * Read exactly n digits. Returns false when fewer are available.
 */
static bool read_fixed(const char **s, size_t *len, size_t n, int *out)
{
    int value = 0;
    size_t i;

    if (*len < n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if ((*s)[i] < '0' || (*s)[i] > '9') {
            return false;
        }
        value = value * 10 + ((*s)[i] - '0');
    }
    *s += n;
    *len -= n;
    *out = value;
    return true;
}

bool dx_http_parse_date(const char *s, size_t len, long long *out_sec)
{
    struct tm tm;
    const char *p = s;
    size_t left = len;
    int value;

    if (s == NULL || out_sec == NULL) {
        return false;
    }
    trim(&p, &left);

    /* "Wdy," - the weekday is validated by shape only. */
    if (left < 5u) {
        return false;
    }
    if (!((p[0] >= 'A' && p[0] <= 'Z') && (p[1] >= 'a' && p[1] <= 'z') &&
          (p[2] >= 'a' && p[2] <= 'z'))) {
        return false;
    }
    if (p[3] != ',' || !is_ows(p[4])) {
        return false;
    }
    p += 4;
    left -= 4;

    /* One or more spaces, then a two digit day. */
    while (left > 0 && is_ows(*p)) {
        p++;
        left--;
    }
    if (!read_fixed(&p, &left, 2u, &value) || value < 1 || value > 31) {
        return false;
    }
    tm.tm_mday = value;

    if (left == 0 || !is_ows(*p)) {
        return false;
    }
    while (left > 0 && is_ows(*p)) {
        p++;
        left--;
    }

    /* Three letter month name. */
    if (left < 3u || !month_from_name(p, 3u, &value)) {
        return false;
    }
    tm.tm_mon = value;
    p += 3;
    left -= 3;

    if (left == 0 || !is_ows(*p)) {
        return false;
    }
    while (left > 0 && is_ows(*p)) {
        p++;
        left--;
    }

    /* Four digit year. */
    if (!read_fixed(&p, &left, 4u, &value) || value < 1970) {
        return false;
    }
    tm.tm_year = value - 1900;

    if (left == 0 || !is_ows(*p)) {
        return false;
    }
    while (left > 0 && is_ows(*p)) {
        p++;
        left--;
    }

    /* HH:MM:SS */
    if (!read_fixed(&p, &left, 2u, &value) || value > 23) {
        return false;
    }
    tm.tm_hour = value;
    if (left == 0 || *p != ':') {
        return false;
    }
    p++;
    left--;
    if (!read_fixed(&p, &left, 2u, &value) || value > 59) {
        return false;
    }
    tm.tm_min = value;
    if (left == 0 || *p != ':') {
        return false;
    }
    p++;
    left--;
    if (!read_fixed(&p, &left, 2u, &value) || value > 60) {
        return false; /* leap seconds are not accepted: they are ambiguous */
    }
    tm.tm_sec = value;

    /* Trailing " GMT" (also accept " UTC" for lenient clients). */
    while (left > 0 && is_ows(*p)) {
        p++;
        left--;
    }
    if (left == 3u && strncmp(p, "GMT", 3u) == 0) {
        left = 0;
    } else if (left == 3u && strncmp(p, "UTC", 3u) == 0) {
        left = 0;
    }
    if (left != 0) {
        return false;
    }

    tm.tm_isdst = 0;
    tm.tm_wday = 0;
    tm.tm_yday = 0;
    {
        time_t when = timegm(&tm);

        if (when == (time_t)-1) {
            return false;
        }
        *out_sec = (long long)when;
    }
    return true;
}

bool dx_http_modified_since(const char *value, size_t len, long long mtime_sec)
{
    long long since = 0;

    if (!dx_http_parse_date(value, len, &since)) {
        return false; /* unparsable date: the header is ignored */
    }
    /* HTTP dates have a one second resolution: compare conservatively. */
    return mtime_sec <= since;
}

bool dx_http_if_range_match(const char *value, size_t len, const char *etag, long long mtime_sec)
{
    long long since = 0;

    if (value == NULL || etag == NULL || len == 0) {
        return false;
    }
    /* If-Range requires a strong validator: no W/ prefix, no "*", no list. */
    if (len >= 2u && value[0] == 'W' && value[1] == '/') {
        return false;
    }
    if (value[0] == '"') {
        return tag_equals(value, len, etag);
    }
    if (dx_http_parse_date(value, len, &since)) {
        /* Exact date match only: a Last-Modified validator must be identical. */
        return since == mtime_sec;
    }
    return false;
}

dx_range_result_t dx_http_range_parse(const char *value, size_t len, uint64_t size, dx_range_t *out)
{
    const char *spec;
    size_t spec_len;
    uint64_t start = 0;
    uint64_t end = 0;
    size_t digits;
    size_t dash;

    if (out == NULL) {
        return DX_RANGE_NONE;
    }
    out->start = 0;
    out->end = 0;
    out->length = 0;
    if (value == NULL || len == 0) {
        return DX_RANGE_NONE;
    }
    trim(&value, &len);
    if (len < 6u) {
        return DX_RANGE_NONE;
    }
    if (strncasecmp(value, "bytes", 5u) != 0 || value[5] != '=') {
        return DX_RANGE_NONE; /* unknown unit: ignore the header */
    }
    spec = value + 6;
    spec_len = len - 6u;
    trim(&spec, &spec_len);
    if (spec_len == 0) {
        return DX_RANGE_NONE;
    }
    if (memchr(spec, ',', spec_len) != NULL) {
        /* Multi-range is deliberately not served; a full 200 closes the
         * overlapping-range attack surface. */
        return DX_RANGE_NONE;
    }

    if (size == 0) {
        return DX_RANGE_UNSATISFIABLE; /* nothing satisfiable in an empty body */
    }

    dash = 0;
    while (dash < spec_len && spec[dash] != '-') {
        dash++;
    }
    if (dash >= spec_len) {
        return DX_RANGE_NONE; /* no dash: invalid */
    }

    if (dash == 0) {
        /* Suffix form "-N": the last N bytes. */
        uint64_t suffix = 0;

        digits = parse_digits(spec + 1, spec_len - 1u, &suffix);
        if (digits == 0 || digits != spec_len - 1u) {
            return DX_RANGE_NONE;
        }
        if (suffix == 0) {
            return DX_RANGE_NONE; /* suffix length zero is invalid */
        }
        if (suffix >= size) {
            out->start = 0;
        } else {
            out->start = size - suffix;
        }
        out->end = size - 1u;
        out->length = size - out->start;
        return DX_RANGE_OK;
    }

    digits = parse_digits(spec, dash, &start);
    if (digits == 0 || digits != dash) {
        return DX_RANGE_NONE;
    }
    if (dash + 1u == spec_len) {
        /* "N-": from N to the end. */
        if (start >= size) {
            return DX_RANGE_UNSATISFIABLE;
        }
        out->start = start;
        out->end = size - 1u;
        out->length = size - start;
        return DX_RANGE_OK;
    }

    digits = parse_digits(spec + dash + 1u, spec_len - dash - 1u, &end);
    if (digits == 0 || digits != spec_len - dash - 1u) {
        return DX_RANGE_NONE;
    }
    if (start > end) {
        return DX_RANGE_NONE; /* inverted range: ignore it */
    }
    if (start >= size) {
        return DX_RANGE_UNSATISFIABLE;
    }
    if (end >= size) {
        end = size - 1u; /* the range is clamped to the representation */
    }
    out->start = start;
    out->end = end;
    out->length = end - start + 1u;
    return DX_RANGE_OK;
}