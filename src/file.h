/*
 * file.h - Document root access: safe path opening, stat, index files,
 * directory redirects and HTML directory listings.
 *
 * Every lookup is relative to a directory fd opened once at start-up, so no
 * request can ever escape the root through the filesystem: openat2 with
 * RESOLVE_BENEATH enforces it in the kernel, and the fallback walks the
 * segments itself with O_NOFOLLOW.
 */
#ifndef DX_FILE_H
#define DX_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

/* Longest redirect Location we build. */
#define DX_LOCATION_MAX 4200u

typedef struct dx_fs dx_fs_t;

typedef enum {
    DX_OPEN_OK = 0,    /* a regular file is open in dx_open_result_t::fd */
    DX_OPEN_REDIRECT,  /* directory without trailing slash; Location built */
    DX_OPEN_LISTING,   /* directory, no index, listing enabled */
    DX_OPEN_FORBIDDEN, /* permission denied */
    DX_OPEN_NOT_FOUND, /* missing, not a regular file, or hidden by policy */
    DX_OPEN_ERROR      /* unexpected system failure */
} dx_open_t;

typedef struct {
    dx_open_t result;
    int fd;         /* owned by the caller; valid only for DX_OPEN_OK */
    struct stat st; /* stat of the served file */
} dx_open_result_t;

/*
 * Open the document root. index_name is the file served for directories,
 * directory_listing enables the listing fallback and follow_symlinks allows
 * symlinks that stay inside the root (RESOLVE_BENEATH still applies).
 * Returns NULL when the root cannot be opened or is not a directory.
 */
dx_fs_t *dx_fs_create(const char *root, const char *index_name, bool directory_listing,
                      bool follow_symlinks);

/* Close the root fd and release the handle. */
void dx_fs_destroy(dx_fs_t *fs);

/* True when the running kernel provides openat2(). */
bool dx_fs_has_openat2(void);

/*
 * Open rel_path (already normalized, relative to the root, no leading slash;
 * "." is the root itself).
 *
 * had_trailing_slash tells whether the request ended with '/'. For a
 * directory reached without one, DX_OPEN_REDIRECT is returned and the
 * redirect target is written into location (length in location_len); the
 * location is always a path with a trailing slash, never a full URL.
 *
 * served_path/served_cap receive the relative path of the file that was
 * actually opened (the index file for a directory), which is what the MIME
 * lookup must use. It may be NULL when the caller does not need it.
 *
 * For a directory with an index file the result is DX_OPEN_OK with the index
 * open; otherwise DX_OPEN_LISTING when listings are enabled, or
 * DX_OPEN_FORBIDDEN.
 */
dx_open_t dx_fs_open(dx_fs_t *fs, const char *rel_path, bool had_trailing_slash, char *location,
                     size_t location_cap, size_t *location_len, char *served_path,
                     size_t served_cap, dx_open_result_t *out);

/*
 * Render an HTML directory listing into out. Returns the number of bytes
 * written, or 0 when the buffer is too small or the directory unreadable.
 * Names are HTML-escaped and percent-encoded for the href.
 */
size_t dx_fs_list(dx_fs_t *fs, const char *rel_path, char *out, size_t cap);

/* HTML-escape text into out; returns the written length or 0 on overflow. */
size_t dx_html_escape(const char *src, size_t len, char *out, size_t cap);

#endif /* DX_FILE_H */
