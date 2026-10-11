/*
 * fuzz_path.c - Fuzz harness for path resolution and the header-driven
 * conditional/range logic.
 *
 * The parser fuzz target covers request framing; this one covers everything
 * that turns a decoded target into a filesystem path and a set of response
 * headers. Traversal, encoded separators, control bytes and overlong paths are
 * all in scope.
 *
 * Build modes: libFuzzer (clang -fsanitize=fuzzer) or the deterministic
 * standalone driver (FUZZ_STANDALONE), which runs in CI with plain gcc.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/http_range.h"
#include "../src/mime.h"
#include "../src/path.h"

/* libFuzzer entry point; also called by the standalone driver below. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void invariant_failed(const char *what)
{
    fprintf(stderr, "fuzz invariant failed: %s\n", what);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    dx_path_t resolved;
    dx_path_opts_t opts;
    dx_range_t range;
    dx_mime_t *mime;
    char content_type[128];

    opts.hidden_files = ((size & 1u) != 0u);

    /* The target is the raw input: it may be anything a client sends. */
    if (dx_path_resolve((const char *)data, size, &opts, &resolved) == 0) {
        size_t i;
        size_t n = strlen(resolved.path);

        /* A resolved path is always relative: never an absolute path and
         * never a bare "..", whatever the input looked like. */
        if (n > 0 && resolved.path[0] == '/') {
            invariant_failed("resolved path is absolute");
        }
        for (i = 0; i < n; i++) {
            if (resolved.path[i] < 0x20u || resolved.path[i] == 0x7fu) {
                invariant_failed("resolved path contains a control byte");
            }
        }
        if (n == 0) {
            invariant_failed("resolved path is empty");
        }
        /* "." is the legitimate name of the root itself. */
        /* No segment may be ".." once the path has been normalized. */
        for (i = 0; i + 1u < n; i++) {
            if (resolved.path[i] == '.' && resolved.path[i + 1u] == '.' &&
                (i == 0 || resolved.path[i - 1u] == '/') &&
                (i + 2u == n || resolved.path[i + 2u] == '/')) {
                invariant_failed("resolved path still contains ..");
            }
        }
    }

    /* MIME mapping must always produce a value or fail cleanly. */
    mime = dx_mime_create(NULL, "application/octet-stream");
    if (mime == NULL) {
        invariant_failed("mime set could not be created");
        return 1;
    }
    if (dx_mime_content_type(mime, resolved.path, content_type, sizeof(content_type)) == 0) {
        content_type[0] = '\0';
    }
    if (content_type[0] == '\0') {
        invariant_failed("content type composed to an empty string");
    }
    dx_mime_destroy(mime);

    /* The same input drives the header-driven paths: Range parsing against a
     * size taken from the input, and ETag matching against an input tag. */
    if (size > 0) {
        uint64_t file_size = (uint64_t)data[0];
        uint64_t mtime = (uint64_t)(size % 1000u);

        if (dx_http_range_parse((const char *)data, size, file_size, &range) == DX_RANGE_OK) {
            if (range.start > range.end || range.length == 0) {
                invariant_failed("range is inconsistent");
            }
            if (range.end >= file_size) {
                invariant_failed("range end exceeds the representation size");
            }
            if (range.length != range.end - range.start + 1u) {
                invariant_failed("range length does not match its bounds");
            }
        }
        {
            char etag[64];

            if (dx_http_etag_format(etag, sizeof(etag), (long long)mtime, file_size) > 0) {
                (void)dx_http_etag_matches((const char *)data, size, etag);
                (void)dx_http_if_range_match((const char *)data, size, etag, (long long)mtime);
            }
        }
        {
            long long when = 0;

            if (dx_http_parse_date((const char *)data, size, &when)) {
                (void)dx_http_modified_since((const char *)data, size, when);
            }
        }
    }
    return 0;
}

/* ----------------------- standalone driver ------------------------------ */

#ifdef FUZZ_STANDALONE

static uint32_t rng_state = 0x2468ace0u;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13u;
    rng_state ^= rng_state >> 17u;
    rng_state ^= rng_state << 5u;
    return rng_state;
}

int main(void)
{
    static const char *seeds[] = {
        "/",
        "/index.html",
        "/a/b/c.txt?v=1",
        "/../../etc/passwd",
        "/%2e%2e%2f%2e%2e%2fetc%2fpasswd",
        "/..%2f..%2fetc",
        "/%00",
        "/.env",
        "/.well-known/acme.txt",
        "http://example.com/a/b",
        "/hello%20world.txt",
        "/a//b///c",
        "/sub/../file",
        "/*",
        "",
    };
    static const size_t n_seeds = sizeof(seeds) / sizeof(seeds[0]);
    unsigned char buf[8192];
    const long iterations = 300000L;

    rng_state = 0x2468ace0u; /* fixed seed: CI stays deterministic */

    for (size_t i = 0; i < n_seeds; i++) {
        LLVMFuzzerTestOneInput((const uint8_t *)seeds[i], strlen(seeds[i]));
    }

    for (long it = 0; it < iterations; it++) {
        size_t len;
        size_t seed_len;
        unsigned rounds;

        /* Start from a seed, extended with filler. */
        {
            const char *seed = seeds[rng_next() % n_seeds];

            seed_len = strlen(seed);
            len = seed_len + (size_t)(rng_next() % 64u);
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            memcpy(buf, seed, seed_len < len ? seed_len : len);
            if (seed_len < len) {
                memset(buf + seed_len, 'a', len - seed_len);
            }
        }

        rounds = (unsigned)(rng_next() % 6u) + 1u;
        for (unsigned m = 0; m < rounds && len > 0u; m++) {
            size_t at = (size_t)(rng_next() % len);

            switch (rng_next() % 5u) {
            case 0: /* escape and separator bytes: the attack surface */
                buf[at] = (unsigned char)("%/..\\;\x00\xff "[rng_next() % 9u]);
                break;
            case 1:
                len = at + 1u;
                break;
            case 2:
                if (len + 16u < sizeof(buf)) {
                    memmove(buf + at + 8u, buf + at, 8u);
                    len += 8u;
                }
                break;
            case 3:
                if (len > 1u) {
                    unsigned char t = buf[at];

                    buf[at] = buf[(at + 1u) % len];
                    buf[(at + 1u) % len] = t;
                }
                break;
            default:
                buf[at] = (unsigned char)rng_next();
                break;
            }
        }
        LLVMFuzzerTestOneInput(buf, len);
    }

    printf("fuzz_path: %ld iterations, no invariant violations\n", iterations);
    return 0;
}

#endif /* FUZZ_STANDALONE */