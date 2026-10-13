/*
 * mime.h - Extension to Content-Type mapping.
 *
 * A built-in table covers the common web extensions; a custom mime.types file
 * (the classic mime.types format: "type ext1 ext2;") can add or override
 * entries.
 *
 * dx_mime_type_for() returns the stable base type owned by the table.
 * dx_mime_content_type() composes the full header value, applying the charset
 * rule from the README (every text type, application/json and JavaScript get
 * "; charset=utf-8"), into a caller supplied buffer.
 */
#ifndef DX_MIME_H
#define DX_MIME_H

#include <stdbool.h>
#include <stddef.h>

typedef struct dx_mime dx_mime_t;

/*
 * Create a mapping set. custom_file may be NULL (built-in table only); a set
 * that exists but failed to read the file simply has no custom entries, which
 * dx_mime_custom_count() reports.
 * default_type is the type for unknown extensions and must not be NULL.
 * Returns NULL only on allocation failure or a NULL default_type.
 */
dx_mime_t *dx_mime_create(const char *custom_file, const char *default_type);

/* Release the set. */
void dx_mime_destroy(dx_mime_t *m);

/*
 * Base type for a path (without charset), owned by the set and valid until
 * dx_mime_destroy(). Never returns NULL.
 */
const char *dx_mime_type_for(const dx_mime_t *m, const char *path);

/*
 * Write the full Content-Type value (type plus charset when required) into
 * out. Returns the number of bytes written excluding the terminator, or 0 when
 * the buffer is too small.
 */
size_t dx_mime_content_type(const dx_mime_t *m, const char *path, char *out, size_t cap);

/* Number of entries loaded from the custom file (0 when none was given). */
size_t dx_mime_custom_count(const dx_mime_t *m);

/*
 * Copy the lowercase extension of path into buf (at most cap-1 bytes).
 * Returns the number of bytes written excluding the terminator, or 0 when the
 * path has no extension or buf is too small.
 */
size_t dx_mime_extension(const char *path, char *buf, size_t cap);

#endif /* DX_MIME_H */
