/*
 * path.h - Request target to filesystem path resolution.
 *
 * Order of operations (README "Path resolution"):
 *   1. strip the query string
 *   2. validate the form (absolute-form authority, leading slash, controls)
 *   3. percent-decode exactly once
 *   4. normalize: collapse "//", drop ".", resolve ".."
 *   5. hidden segments are refused unless allowed
 *
 * Decoding happens exactly once and normalization runs on the decoded bytes,
 * so "%2e%2e%2f" is recognised as "../" and can never slip past the checks.
 */
#ifndef DX_PATH_H
#define DX_PATH_H

#include <stdbool.h>
#include <stddef.h>

/* Longest resolved path we accept; longer requests are rejected. */
#define DX_PATH_MAX 4096u

typedef struct {
    bool hidden_files; /* true: dot segments are served; false: they 404 */
} dx_path_opts_t;

/*
 * Result of a successful resolution.
 *
 * path is relative to the document root and never starts with a slash ("."
 * is the root itself), so it can be handed to openat2() with RESOLVE_BENEATH.
 * had_trailing_slash keeps the information needed for the 301 redirect.
 */
typedef struct {
    char path[DX_PATH_MAX];
    bool had_trailing_slash;
} dx_path_t;

/*
 * Resolve target[0..len) into out.
 *
 * Returns 0 on success, 400 for malformed targets or escapes above the root,
 * and 404 for hidden segments (when hidden_files is false). Returns the same
 * codes for a path longer than DX_PATH_MAX.
 */
int dx_path_resolve(const char *target, size_t len, const dx_path_opts_t *opts, dx_path_t *out);

#endif /* DX_PATH_H */