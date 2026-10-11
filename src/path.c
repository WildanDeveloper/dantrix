#include "path.h"

#include <strings.h>
#include <string.h>

#define DX_PATH_BAD_REQUEST 400
#define DX_PATH_NOT_FOUND 404

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

/*
 * Percent-decode exactly once into dst (capacity dst_cap). Returns the decoded
 * length, or -1 on malformed escapes, an encoded NUL, or a buffer overflow.
 * A decode pass never produces a second decode pass: every '%' written here is
 * a literal percent sign.
 */
static int percent_decode(const char *src, size_t len, char *dst, size_t dst_cap)
{
    size_t i = 0;
    size_t o = 0;

    while (i < len) {
        unsigned char c = (unsigned char)src[i];

        if (c == '%') {
            int hi;
            int lo;

            if (i + 2u >= len) {
                return -1; /* truncated escape */
            }
            hi = hex_value((unsigned char)src[i + 1u]);
            lo = hex_value((unsigned char)src[i + 2u]);
            if (hi < 0 || lo < 0) {
                return -1; /* not hex digits */
            }
            c = (unsigned char)((hi << 4) | lo);
            if (c == 0x00u) {
                return -1; /* encoded NUL: never valid in a path */
            }
            i += 3u;
        } else {
            i += 1u;
        }
        /*
         * Validate the decoded byte, not just the raw one: "%0a" would
         * otherwise smuggle a newline into a path and from there into logs,
         * headers or directory listings.
         */
        if (c < 0x20u || c == 0x7fu) {
            return -1;
        }
        if (o + 1u >= dst_cap) {
            return -1; /* decoded path longer than the accepted maximum */
        }
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return (int)o;
}

/*
 * Skip the scheme and authority of an absolute-form target
 * ("http://host/path"), returning the offset where the path begins. Returns 0
 * when the target is not in absolute form (scheme names are matched
 * case-insensitively per RFC 3986).
 */
static size_t skip_authority(const char *target, size_t len)
{
    static const char *const schemes[] = {"https://", "http://"};
    size_t s;

    for (s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++) {
        size_t plen = strlen(schemes[s]);
        size_t i;

        if (len <= plen || strncasecmp(target, schemes[s], plen) != 0) {
            continue;
        }
        i = plen;
        while (i < len && target[i] != '/') {
            i++; /* skip host and optional port */
        }
        return i;
    }
    return 0;
}

int dx_path_resolve(const char *target, size_t len, const dx_path_opts_t *opts, dx_path_t *out)
{
    char decoded[DX_PATH_MAX + 1u];
    size_t i;
    int decoded_len;

    if (target == NULL || out == NULL) {
        return DX_PATH_BAD_REQUEST;
    }
    if (len == 0) {
        return DX_PATH_BAD_REQUEST;
    }

    /* 1. Strip the query string. */
    for (i = 0; i < len; i++) {
        if (target[i] == '?') {
            len = i;
            break;
        }
    }
    /* 2. Absolute-form: drop scheme and authority. */
    i = skip_authority(target, len);
    target += i;
    len -= i;

    if (len == 0 || target[0] != '/') {
        /* Not a valid origin-form target: "*" and authority-only forms
         * never reach the static handler. */
        return DX_PATH_BAD_REQUEST;
    }

    /* 2. Reject control characters and DEL in the raw target. */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)target[i];

        if (c < 0x21u || c == 0x7fu) {
            return DX_PATH_BAD_REQUEST;
        }
    }

    /* 3. Percent-decode exactly once. */
    decoded_len = percent_decode(target + 1u, len - 1u, decoded, sizeof(decoded));
    if (decoded_len < 0) {
        return DX_PATH_BAD_REQUEST;
    }
    if ((size_t)decoded_len >= DX_PATH_MAX) {
        return DX_PATH_BAD_REQUEST;
    }

    /* 4. Normalize: collapse "//", drop ".", resolve "..". */
    {
        char norm[DX_PATH_MAX + 1u];
        bool trailing_slash = false;
        size_t o = 0;
        size_t seg_start = 0;
        size_t seg_len;

        for (i = 0; i <= (size_t)decoded_len; i++) {
            bool at_end = (i == (size_t)decoded_len);

            if (!at_end && decoded[i] != '/') {
                continue;
            }
            seg_len = i - seg_start;
            if (seg_len == 0) {
                /* Empty segment from "//" or a trailing slash. */
                if (at_end && o > 0) {
                    trailing_slash = true;
                }
            } else if (seg_len == 1u && decoded[seg_start] == '.') {
                /* "." contributes nothing. */
            } else if (seg_len == 2u && decoded[seg_start] == '.' &&
                       decoded[seg_start + 1u] == '.') {
                if (o == 0) {
                    return DX_PATH_BAD_REQUEST; /* escape above the root */
                }
                /* Drop the previous segment. */
                while (o > 0 && norm[o - 1u] != '/') {
                    o--;
                }
                if (o > 0) {
                    o--; /* remove the separator too */
                }
            } else {
                size_t j;

                /* 5. Hidden segments are refused unless explicitly allowed. */
                if (decoded[seg_start] == '.' && (opts == NULL || !opts->hidden_files)) {
                    bool well_known =
                        (seg_len == 11u && strncmp(decoded + seg_start, ".well-known", 11u) == 0);

                    if (!well_known) {
                        return DX_PATH_NOT_FOUND;
                    }
                }
                if (o > 0) {
                    if (o + 1u >= sizeof(norm)) {
                        return DX_PATH_BAD_REQUEST;
                    }
                    norm[o++] = '/';
                }
                for (j = 0; j < seg_len; j++) {
                    if (o + 1u >= sizeof(norm)) {
                        return DX_PATH_BAD_REQUEST;
                    }
                    norm[o++] = decoded[seg_start + j];
                }
            }
            seg_start = i + 1u;
        }
        norm[o] = '\0';
        if (o == 0) {
            /* The root itself, which always ends with a slash: serving the
             * index directly must not trigger a redirect to "/./". */
            out->had_trailing_slash = true;
            out->path[0] = '.';
            out->path[1] = '\0';
            return 0;
        }
        out->had_trailing_slash = trailing_slash;
        memcpy(out->path, norm, o + 1u);
    }
    return 0;
}
