#include "file.h"

#include "fdcount.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DX_INDEX_MAX 64u
#define DX_LISTING_MAX 512u
/*
 * Room for the escaped heading path. The heading is cosmetic, so an
 * over-long path is truncated instead of failing the whole listing: every
 * byte may expand to six characters (&quot; and friends).
 */
#define DX_LISTING_TITLE_MAX 1024u

struct dx_fs {
    int root_fd;
    char index_name[DX_INDEX_MAX];
    bool directory_listing;
    bool follow_symlinks;
};

/* openat2 is not exposed by every libc; call it through the syscall ABI. */
static int sys_openat2(int dirfd, const char *path, struct open_how *how, size_t size)
{
    return (int)syscall(SYS_openat2, dirfd, path, how, size);
}

bool dx_fs_has_openat2(void)
{
    struct open_how how;
    int fd;

    memset(&how, 0, sizeof(how));
    how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH;
    fd = sys_openat2(AT_FDCWD, ".", &how, sizeof(how));
    if (fd >= 0) {
        (void)close(fd);
        return true;
    }
    /* ENOSYS: old kernel. Anything else is a policy failure of this probe and
     * must not disable the syscall for real requests. */
    return errno != ENOSYS;
}

bool dx_fs_openat2_blocked(void)
{
    struct open_how how;

    memset(&how, 0, sizeof(how));
    how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH;
    {
        int fd = sys_openat2(AT_FDCWD, ".", &how, sizeof(how));

        if (fd >= 0) {
            /* The probe's own descriptor must be closed: it was leaked once per
             * call (measured: 4 -> 9 after five calls), and it made the
             * descriptor accounting drift by one for the process lifetime. */
            (void)close(fd);
            return false; /* it actually works */
        }
    }
    return errno == EPERM;
}

/* Percent-encode a file name or path for use in a URI; defined further down. */

/* Map an open failure onto the response classes of the README. */
static dx_open_t classify_errno(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
    case ELOOP:
    case ENAMETOOLONG:
    case EXDEV:  /* the kernel refused to leave the root */
    case EINVAL: /* the resolve flags were rejected */
        return DX_OPEN_NOT_FOUND;
    case EACCES:
    case EPERM:
        return DX_OPEN_FORBIDDEN;
    default:
        return DX_OPEN_ERROR;
    }
}

/*
 * Open rel_path below root_fd.
 *
 * Primary path: openat2 with RESOLVE_BENEATH (plus RESOLVE_NO_SYMLINKS when
 * symlinks are refused), which makes escapes above the root impossible by
 * construction.
 *
 * Fallback, only when the kernel has no openat2 (ENOSYS) or rejects the
 * resolve flags (EINVAL): walk the segments one by one with O_NOFOLLOW. The
 * fallback refuses symlinks even when follow_symlinks is on, because without
 * RESOLVE_BENEATH there is no kernel guarantee that a followed link stays
 * inside the root; degrading to a refusal is the only safe choice.
 *
 * EXDEV and ELOOP are answers from a working kernel, never a reason to fall
 * back: EXDEV means the resolve rules rejected the path and ELOOP that a
 * refused symlink was hit. Both stay refusals.
 */
static int fs_open_beneath(dx_fs_t *fs, const char *rel_path, struct stat *st)
{
    struct open_how how;
    int fd;

    memset(&how, 0, sizeof(how));
    /*
     * O_NONBLOCK is not about regular files (it is a no-op for them): it stops
     * open() from blocking forever on a FIFO in the document root. A worker is
     * a single-threaded event loop, so one blocking open() freezes every
     * connection it owns -- and a FIFO is trivial to place in an upload
     * directory, or to hard-link into the tree (RESOLVE_BENEATH cannot stop a
     * hard link). Reproduced against v1.0.3: GET /pipe wedged the worker and
     * the server stopped answering. The S_ISREG check happens after the open,
     * so it could never run.
     */
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH;
    if (!fs->follow_symlinks) {
        how.resolve |= RESOLVE_NO_SYMLINKS;
        how.flags |= O_NOFOLLOW;
    }
    fd = sys_openat2(fs->root_fd, rel_path, &how, sizeof(how));
    if (fd >= 0) {
        if (fstat(fd, st) != 0) {
            int saved = errno;

            (void)close(fd);
            errno = saved;
            return -1;
        }
        return fd;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }

    {
        int dir_fd;
        const char *p = rel_path;

        if (p[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        dir_fd = dup(fs->root_fd);
        if (dir_fd < 0) {
            return -1;
        }
        while (*p != '\0') {
            char segment[NAME_MAX + 1u];
            const char *slash = strchr(p, '/');
            size_t seg_len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
            int next;

            if (seg_len == 0) {
                while (*p == '/') {
                    p++;
                }
                continue;
            }
            if (seg_len > NAME_MAX) {
                (void)close(dir_fd);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(segment, p, seg_len);
            segment[seg_len] = '\0';

            /*
             * The fallback must be as strict as RESOLVE_BENEATH, and it was
             * not: ".." was passed straight to openat() and walked out of the
             * root, and intermediate components were opened without
             * O_DIRECTORY/O_NOFOLLOW, so a symlinked directory was followed.
             * Reproduced on a build whose openat2() was forced to fail:
             * dx_fs_open(fs, "../outside.txt") returned the outside file.
             * Reject the dot segments here; the caller's normalisation is not
             * this function's guarantee.
             */
            if (strcmp(segment, ".") == 0 || strcmp(segment, "..") == 0) {
                (void)close(dir_fd);
                errno = EXDEV;
                return -1;
            }
            /* O_NONBLOCK for the same FIFO reason as the primary path;
             * O_DIRECTORY unless this is the final component. */
            next = openat(dir_fd, segment,
                          O_RDONLY | O_CLOEXEC | O_NONBLOCK | (unsigned)O_NOFOLLOW |
                              ((*p == '\0' || slash == NULL) ? 0u : (unsigned)O_DIRECTORY));
            if (next < 0) {
                int saved = errno;

                (void)close(dir_fd);
                errno = saved;
                return -1;
            }
            (void)close(dir_fd);
            dir_fd = next;
            p += seg_len;
        }
        if (fstat(dir_fd, st) != 0) {
            int saved = errno;

            (void)close(dir_fd);
            errno = saved;
            return -1;
        }
        return dir_fd;
    }
}

/* Join a relative directory and a child name without a double separator.
 * Returns 0 on success, -1 when the result would not fit. */
static int join_path(char *buf, size_t cap, const char *dir, const char *name)
{
    int n;

    if (strcmp(dir, ".") == 0) {
        n = snprintf(buf, cap, "%s", name);
    } else {
        n = snprintf(buf, cap, "%s/%s", dir, name);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Copy src into dst, truncating safely. Returns the copied length. */
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

dx_fs_t *dx_fs_create(const char *root, const char *index_name, bool directory_listing,
                      bool follow_symlinks)
{
    dx_fs_t *fs;
    struct stat st;
    int root_fd;

    if (root == NULL || root[0] == '\0') {
        return NULL;
    }
    root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        return NULL;
    }
    dx_fdcount_opened(DX_FD_MISC); /* document root: held for the process life */
    if (fstat(root_fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        (void)close(root_fd);
        return NULL;
    }
    fs = (dx_fs_t *)calloc(1, sizeof(*fs));
    if (fs == NULL) {
        (void)close(root_fd);
        return NULL;
    }
    fs->root_fd = root_fd;
    fs->directory_listing = directory_listing;
    fs->follow_symlinks = follow_symlinks;
    if (index_name != NULL && index_name[0] != '\0') {
        size_t n = strlen(index_name);

        if (n >= sizeof(fs->index_name)) {
            (void)close(root_fd);
            free(fs);
            return NULL;
        }
        memcpy(fs->index_name, index_name, n + 1u);
    }
    return fs;
}

void dx_fs_destroy(dx_fs_t *fs)
{
    if (fs == NULL) {
        return;
    }
    if (fs->root_fd >= 0) {
        (void)close(fs->root_fd);
        dx_fdcount_closed(DX_FD_MISC);
    }
    free(fs);
}

dx_open_t dx_fs_open(dx_fs_t *fs, const char *rel_path, bool had_trailing_slash, char *location,
                     size_t location_cap, size_t *location_len, char *served_path,
                     size_t served_cap, dx_open_result_t *out)
{
    struct stat st;
    int fd;

    if (fs == NULL || rel_path == NULL || out == NULL) {
        return DX_OPEN_ERROR;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    if (location_len != NULL) {
        *location_len = 0;
    }
    if (served_path != NULL && served_cap > 0) {
        served_path[0] = '\0';
    }

    fd = fs_open_beneath(fs, rel_path, &st);
    if (fd < 0) {
        return classify_errno(errno);
    }

    if (S_ISDIR(st.st_mode)) {
        (void)close(fd);
        /* A directory requested without a trailing slash is redirected. */
        if (!had_trailing_slash) {
            if (location != NULL) {
                /*
                 * rel_path is the percent-decoded name, so it is re-encoded
                 * here: a raw space, a '#' or a '?' would either make the
                 * Location an invalid URI-reference or point at a different
                 * resource than the one being redirected to.
                 */
                char encoded[DX_LOCATION_MAX];
                size_t n = dx_url_encode(rel_path, strlen(rel_path), encoded, sizeof(encoded) - 2u);
                int w;

                if (n == 0) {
                    return DX_OPEN_ERROR; /* cannot be represented */
                }
                encoded[n] = '\0';
                w = snprintf(location, location_cap, "/%s/", encoded);
                if (w < 0 || (size_t)w >= location_cap || location_len == NULL) {
                    /*
                     * The caller's buffer is too small to hold the redirect.
                     * Returning DX_OPEN_REDIRECT anyway would emit an empty
                     * "Location:" header, which is worse than a 500: the
                     * client would follow a redirect to the same resource
                     * forever. Report the failure instead.
                     */
                    return DX_OPEN_ERROR;
                }
                *location_len = (size_t)w;
            }
            if (served_path != NULL && served_cap > 0) {
                (void)copy_bounded(served_path, served_cap, rel_path);
            }
            return DX_OPEN_REDIRECT;
        }
        /* Serve the index file when present. */
        if (fs->index_name[0] != '\0') {
            char child[DX_LOCATION_MAX];

            if (join_path(child, sizeof(child), rel_path, fs->index_name) == 0) {
                int index_fd = fs_open_beneath(fs, child, &st);

                if (index_fd >= 0) {
                    if (S_ISREG(st.st_mode)) {
                        out->fd = index_fd;
                        out->st = st;
                        dx_fdcount_opened(DX_FD_FILE);
                        if (served_path != NULL && served_cap > 0) {
                            (void)copy_bounded(served_path, served_cap, child);
                        }
                        return DX_OPEN_OK;
                    }
                    (void)close(index_fd);
                }
            }
        }
        /* No usable index: list the directory when allowed. */
        if (fs->directory_listing) {
            if (served_path != NULL && served_cap > 0) {
                (void)copy_bounded(served_path, served_cap, rel_path);
            }
            return DX_OPEN_LISTING;
        }
        return DX_OPEN_FORBIDDEN;
    }

    if (!S_ISREG(st.st_mode)) {
        /* Sockets, FIFOs and devices are not servable. */
        (void)close(fd);
        return DX_OPEN_NOT_FOUND;
    }
    out->fd = fd;
    out->st = st;
    dx_fdcount_opened(DX_FD_FILE);
    if (served_path != NULL && served_cap > 0) {
        (void)copy_bounded(served_path, served_cap, rel_path);
    }
    return DX_OPEN_OK;
}

size_t dx_html_escape(const char *src, size_t len, char *out, size_t cap)
{
    static const char *const specials[] = {"&lt;", "&gt;", "&amp;", "&quot;", "&#39;"};
    size_t o = 0;

    if (src == NULL || out == NULL || cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *rep = NULL;
        unsigned char plain = c;

        switch (c) {
        case '<':
            rep = specials[0];
            break;
        case '>':
            rep = specials[1];
            break;
        case '&':
            rep = specials[2];
            break;
        case '"':
            rep = specials[3];
            break;
        case '\'':
            rep = specials[4];
            break;
        default:
            break;
        }
        if (rep != NULL) {
            size_t rep_len = strlen(rep);

            if (o + rep_len >= cap) {
                return 0;
            }
            memcpy(out + o, rep, rep_len);
            o += rep_len;
            continue;
        }
        if (c < 0x20u || c == 0x7fu) {
            continue; /* control bytes never appear in a listing */
        }
        if (o + 1u >= cap) {
            return 0;
        }
        out[o++] = (char)plain;
    }
    out[o] = '\0';
    return o;
}

/* Percent-encode a file name for the href attribute. */
/*
 * Percent-encode src into out, leaving only unreserved characters and '/'.
 * Returns the written length, or 0 when the output would not fit (never a
 * partial escape: a half-written escape could leave a quote or a header
 * unbalanced, so the caller gets a refusal instead).
 *
 * Public because the security claims depend on it: it is what keeps a request
 * target from carrying CR/LF into a Location header, and it is fuzzed directly
 * by fuzz/fuzz_escape.c.
 */
size_t dx_url_encode(const char *src, size_t len, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' || c == '/';

        if (safe) {
            if (o + 1u >= cap) {
                return 0;
            }
            out[o++] = (char)c;
        } else {
            if (o + 3u >= cap) {
                return 0;
            }
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xfu];
            out[o++] = hex[c & 0xfu];
        }
    }
    out[o] = '\0';
    return o;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Is child a directory? Returns 1, 0, or -1 when it cannot be determined. */
static int child_is_dir(dx_fs_t *fs, const char *rel_path, const char *name)
{
    char probe[DX_LOCATION_MAX];
    struct stat st;
    int fd;

    if (join_path(probe, sizeof(probe), rel_path, name) != 0) {
        return -1;
    }
    fd = fs_open_beneath(fs, probe, &st);
    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

size_t dx_fs_list(dx_fs_t *fs, const char *rel_path, char *out, size_t cap)
{
    /* Zero-initialised so the cleanup loop is safe after an early break. */
    char *names[DX_LISTING_MAX] = {NULL};
    size_t count = 0;
    struct stat st;
    DIR *dir;
    int fd;
    const struct dirent *ent;
    size_t o;
    int n;

    if (fs == NULL || rel_path == NULL || out == NULL || cap == 0) {
        return 0;
    }
    fd = fs_open_beneath(fs, rel_path, &st);
    if (fd < 0 || !S_ISDIR(st.st_mode)) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return 0;
    }
    dir = fdopendir(fd);
    if (dir == NULL) {
        (void)close(fd);
        return 0;
    }

    while ((ent = readdir(dir)) != NULL && count < DX_LISTING_MAX) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        names[count] = strdup(ent->d_name);
        if (names[count] == NULL) {
            break;
        }
        count++;
    }
    (void)closedir(dir); /* also closes fd */

    qsort(names, count, sizeof(names[0]), name_cmp);

    /*
     * The heading carries the requested path, which is attacker controlled:
     * a directory called "<img src=x onerror=...>" must not become markup.
     * Escape it, and fall back to a neutral heading when it does not fit.
     */
    {
        char title[DX_LISTING_TITLE_MAX];
        size_t path_len = strlen(rel_path);
        size_t room = (sizeof(title) - 1u) / 6u;

        if (path_len > room) {
            path_len = room; /* truncated heading, not a failed listing */
        }
        if (dx_html_escape(rel_path, path_len, title, sizeof(title)) == 0) {
            (void)copy_bounded(title, sizeof(title), "listing");
        }
        n = snprintf(out, cap,
                     "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
                     "<title>Index of /%s</title></head><body><h1>Index of /%s</h1><ul>\n",
                     title, title);
    }
    if (n < 0 || (size_t)n >= cap) {
        o = 0;
    } else {
        o = (size_t)n;
    }

    bool truncated = false;

    for (size_t i = 0; i < count && o > 0; i++) {
        /* Worst case: every byte expands to six characters (&quot;). */
        char esc[NAME_MAX * 6u + 1u];
        char url[NAME_MAX * 3u + 2u];
        size_t name_len = strlen(names[i]);
        int is_dir = child_is_dir(fs, rel_path, names[i]);

        /* An entry that cannot be probed is still worth listing; only the
         * trailing slash marker is lost. */
        if (is_dir < 0) {
            is_dir = 0;
        }
        if (dx_html_escape(names[i], name_len, esc, sizeof(esc)) == 0 ||
            dx_url_encode(names[i], name_len, url, sizeof(url)) == 0) {
            free(names[i]);
            names[i] = NULL;
            continue;
        }
        n = snprintf(out + o, cap - o, "<li><a href=\"%s%s\">%s%s</a></li>\n", url,
                     is_dir ? "/" : "", esc, is_dir ? "/" : "");
        if (n < 0 || (size_t)n >= cap - o) {
            /*
             * The page is full. Stop here and say so instead of returning 0:
             * the caller maps 0 to 403 Forbidden, so before this a directory
             * with enough entries became permanently un-listable (verified:
             * 400 entries of ~60 characters -> 403). A truncated but honest
             * page is better than a status code that says "forbidden" about a
             * directory the server is configured to list.
             */
            free(names[i]);
            names[i] = NULL;
            truncated = true;
            break;
        }
        o += (size_t)n;
        free(names[i]);
        names[i] = NULL;
    }
    /* Any entries not reached after a break are still freed. */
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }

    if (o == 0) {
        return 0;
    }
    if (truncated) {
        int t =
            snprintf(out + o, cap - o,
                     "<li><em>(listing truncated at %u KiB; use a smaller directory)</em></li>\n",
                     (unsigned)(cap / 1024u));
        if (t > 0 && (size_t)t < cap - o) {
            o += (size_t)t;
        }
    }
    n = snprintf(out + o, cap - o, "</ul><hr><address>Dantrix</address></body></html>\n");
    if (n < 0 || (size_t)n >= cap - o) {
        /*
         * Not even the footer fits. Close the document with the bytes that are
         * left rather than returning 0 (which the caller reads as "forbidden").
         */
        static const char close_doc[] = "</ul></body></html>\n";
        size_t room = cap - o;

        if (room > sizeof(close_doc) - 1u) {
            memcpy(out + o, close_doc, sizeof(close_doc) - 1u);
            o += sizeof(close_doc) - 1u;
        }
        return o;
    }
    return o + (size_t)n;
}
