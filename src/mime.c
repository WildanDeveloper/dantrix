#include "mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Longest extension and type we accept, including the terminator. */
#define DX_MIME_EXT_MAX 16u
#define DX_MIME_TYPE_MAX 96u
#define DX_MIME_CUSTOM_MAX 256u

typedef struct {
    char ext[DX_MIME_EXT_MAX];
    char type[DX_MIME_TYPE_MAX];
} dx_mime_entry_t;

struct dx_mime {
    dx_mime_entry_t custom[DX_MIME_CUSTOM_MAX];
    size_t custom_count;
    char default_type[DX_MIME_TYPE_MAX];
};

/*
 * The built-in table. The charset suffix is never stored here: it is applied
 * by dx_mime_content_type() so that one rule covers built-ins and custom
 * entries alike.
 */
static const dx_mime_entry_t mime_builtin[] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"shtml", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"mjs", "text/javascript"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"xml", "application/xml"},
    {"txt", "text/plain"},
    {"log", "text/plain"},
    {"csv", "text/csv"},
    {"tsv", "text/tab-separated-values"},
    {"md", "text/markdown"},
    {"markdown", "text/markdown"},
    {"yaml", "application/yaml"},
    {"yml", "application/yaml"},
    {"toml", "application/toml"},
    {"rst", "text/x-rst"},
    {"ics", "text/calendar"},
    {"vcf", "text/vcard"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"jpe", "image/jpeg"},
    {"gif", "image/gif"},
    {"bmp", "image/bmp"},
    {"ico", "image/vnd.microsoft.icon"},
    {"svg", "image/svg+xml"},
    {"svgz", "image/svg+xml"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"heic", "image/heic"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"eot", "application/vnd.ms-fontobject"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"bz2", "application/x-bzip2"},
    {"xz", "application/x-xz"},
    {"zst", "application/zstd"},
    {"tar", "application/x-tar"},
    {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"},
    {"wasm", "application/wasm"},
    {"bin", "application/octet-stream"},
    {"exe", "application/octet-stream"},
    {"dll", "application/octet-stream"},
    {"so", "application/octet-stream"},
    {"deb", "application/octet-stream"},
    {"rpm", "application/x-rpm"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"oga", "audio/ogg"},
    {"wav", "audio/wav"},
    {"flac", "audio/flac"},
    {"m4a", "audio/mp4"},
    {"aac", "audio/aac"},
    {"mp4", "video/mp4"},
    {"m4v", "video/mp4"},
    {"webm", "video/webm"},
    {"ogv", "video/ogg"},
    {"mov", "video/quicktime"},
    {"avi", "video/x-msvideo"},
    {"mkv", "video/x-matroska"},
    {"webmanifest", "application/manifest+json"},
    {"appcache", "text/cache-manifest"},
    {"ts", "video/mp2t"},
};
static const size_t mime_builtin_count = sizeof(mime_builtin) / sizeof(mime_builtin[0]);

/* True when the type must be served with charset=utf-8. */
static bool wants_charset(const char *type)
{
    if (strncmp(type, "text/", 5u) == 0) {
        return true;
    }
    if (strcmp(type, "application/json") == 0 || strcmp(type, "application/yaml") == 0 ||
        strcmp(type, "application/manifest+json") == 0) {
        return true;
    }
    return false;
}

static size_t copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0u) {
        return 0; /* `cap - 1u` would wrap: nothing can be written anyway */
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t dx_mime_extension(const char *path, char *buf, size_t cap)
{
    const char *slash;
    const char *dot;
    size_t len;
    size_t i;

    if (path == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    slash = strrchr(path, '/');
    dot = strrchr((slash != NULL) ? slash + 1 : path, '.');
    if (dot == NULL) {
        return 0;
    }
    len = strlen(dot + 1);
    if (len == 0 || len >= cap || len >= DX_MIME_EXT_MAX) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = dot[1u + i];

        buf[i] = ((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    }
    buf[len] = '\0';
    return len;
}

/* Replace an existing custom entry for ext, or append it. */
static void mime_set_custom(dx_mime_t *m, const char *ext, const char *type)
{
    size_t i;

    for (i = 0; i < m->custom_count; i++) {
        if (strcmp(m->custom[i].ext, ext) == 0) {
            (void)copy_bounded(m->custom[i].type, sizeof(m->custom[i].type), type);
            return;
        }
    }
    if (m->custom_count >= DX_MIME_CUSTOM_MAX) {
        return; /* table full: bounded, never grows without a limit */
    }
    (void)copy_bounded(m->custom[m->custom_count].ext, sizeof(m->custom[m->custom_count].ext), ext);
    (void)copy_bounded(m->custom[m->custom_count].type, sizeof(m->custom[m->custom_count].type),
                       type);
    m->custom_count++;
}

/*
 * Parse one mime.types style line: "type ext1 ext2 ext3;". Returns false when
 * the line carries no usable mapping.
 */
/* Copy strtok_r() results through a const pointer: the tokens are never
 * modified here, only copied into the table. */
static bool mime_parse_line(dx_mime_t *m, char *line)
{
    char *save = NULL;
    char *token = strtok_r(line, " \t\r", &save);
    const char *type = token;

    if (token == NULL || token[0] == '\0') {
        return false;
    }
    while ((token = strtok_r(NULL, " \t\r", &save)) != NULL) {
        if (token[0] == '\0') {
            continue;
        }
        mime_set_custom(m, token, type);
    }
    return true;
}

/*
 * Load a custom table. data is a mutable copy of the whole file; it is
 * processed line by line and never retained by the mapping set.
 */
static void mime_load_custom(dx_mime_t *m, char *data, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        size_t end = pos;
        char *hash;
        char *semi;

        while (end < len && data[end] != '\n') {
            end++;
        }
        if (end == pos) {
            pos = end + 1u;
            continue; /* blank line: nothing to map, and it has no terminator */
        }
        /*
         * The buffer is allocated with one spare byte so the last line of a
         * file that ends exactly at the size cap can be terminated here.
         * Terminating the newline itself is fine; data[len] is not written.
         */
        data[end] = '\0';
        /* Comments end the line; the trailing semicolon is optional. */
        hash = strchr(data + pos, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        semi = strrchr(data + pos, ';');
        if (semi != NULL) {
            *semi = '\0';
        }
        (void)mime_parse_line(m, data + pos);
        pos = end + 1u;
    }
}

dx_mime_t *dx_mime_create(const char *custom_file, const char *default_type)
{
    dx_mime_t *m;

    if (default_type == NULL) {
        return NULL;
    }
    m = (dx_mime_t *)calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    (void)copy_bounded(m->default_type, sizeof(m->default_type), default_type);

    if (custom_file != NULL && custom_file[0] != '\0') {
        FILE *f = fopen(custom_file, "rb");

        if (f != NULL) {
            /* cap bytes are read; one extra byte keeps room for the
             * terminator that mime_load_custom() writes after the last line. */
            size_t cap = 8192u;
            char *buf = (char *)malloc(cap + 1u);

            if (buf != NULL) {
                size_t len = 0;
                size_t got;

                while ((got = fread(buf + len, 1u, cap - len, f)) > 0) {
                    char *bigger;

                    len += got;
                    if (len < cap) {
                        continue;
                    }
                    if (cap >= 1024u * 1024u) {
                        break; /* refuse pathological files */
                    }
                    cap *= 2u;
                    bigger = (char *)realloc(buf, cap + 1u);
                    if (bigger == NULL) {
                        break;
                    }
                    buf = bigger;
                }
                mime_load_custom(m, buf, len);
                free(buf);
            }
            (void)fclose(f);
        }
    }
    return m;
}

void dx_mime_destroy(dx_mime_t *m)
{
    free(m);
}

size_t dx_mime_custom_count(const dx_mime_t *m)
{
    return (m == NULL) ? 0u : m->custom_count;
}

const char *dx_mime_type_for(const dx_mime_t *m, const char *path)
{
    char ext[DX_MIME_EXT_MAX];

    if (m == NULL || path == NULL) {
        return "application/octet-stream";
    }
    if (dx_mime_extension(path, ext, sizeof(ext)) > 0) {
        /* Custom entries win over the built-ins, matching nginx behaviour. */
        for (size_t i = 0; i < m->custom_count; i++) {
            if (strcmp(m->custom[i].ext, ext) == 0) {
                return m->custom[i].type;
            }
        }
        for (size_t i = 0; i < mime_builtin_count; i++) {
            if (strcmp(mime_builtin[i].ext, ext) == 0) {
                return mime_builtin[i].type;
            }
        }
    }
    return m->default_type;
}

size_t dx_mime_content_type(const dx_mime_t *m, const char *path, char *out, size_t cap)
{
    const char *type;
    int w;

    if (out == NULL || cap == 0) {
        return 0;
    }
    type = dx_mime_type_for(m, path);
    if (!wants_charset(type)) {
        /*
         * copy_bounded() truncates silently; the documented contract of this
         * function is "0 when the buffer is too small", and the caller uses
         * that to avoid emitting a broken Content-Type.
         */
        if (strlen(type) >= cap) {
            out[0] = '\0';
            return 0;
        }
        return copy_bounded(out, cap, type);
    }
    w = snprintf(out, cap, "%s; charset=utf-8", type);
    if (w < 0 || (size_t)w >= cap) {
        /* Encoding failure or truncation: the caller must not emit a partial
         * Content-Type, so report "did not fit". */
        out[0] = '\0';
        return 0;
    }
    return (size_t)w;
}
