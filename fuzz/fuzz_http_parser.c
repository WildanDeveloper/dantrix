/*
 * fuzz_http_parser.c - Fuzz harness for the HTTP parser and response builder.
 *
 * Two build modes:
 *   - libFuzzer: clang -fsanitize=fuzzer,address fuzz/fuzz_http_parser.c ...
 *   - standalone (default, works with gcc): a deterministic in-process loop
 *     derives candidates from a few seed requests with a fixed-seed PRNG, so
 *     CI runs the same sequence on every machine without any dependency.
 *
 * Invariants checked on every input:
 *   1. The parser never reads out of bounds, never hangs and never crashes
 *      (ASan/UBSan guard this when linked in).
 *   2. Feeding the same request in two halves reaches the same verdict as
 *      feeding it whole (incremental parsing is semantically transparent).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/http_parser.h"
#include "../src/http_response.h"

/* Abort on an invariant violation: this is what fails the run. */
static void invariant_failed(const char *what)
{
    fprintf(stderr, "fuzz invariant failed: %s\n", what);
    abort();
}

/* libFuzzer entry point; also called by the standalone driver below. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    dx_http_limits_t limits;
    dx_http_parser_t p;
    dx_parse_result_t whole;
    size_t consumed = 0;

    dx_http_limits_default(&limits);
    dx_http_parser_init(&p, &limits);
    whole = dx_http_parser_feed(&p, data, size, &consumed);

    if (whole == DX_PARSE_OK) {
        char out[2048];
        int status = 0;
        const char *extra = NULL;

        if (consumed > size) {
            invariant_failed("consumed exceeds the fed length");
        }
        (void)dx_http_dispatch_method(&p.req, data, &status, &extra);
        (void)dx_http_response_build(out, sizeof(out), 200, NULL, "text/plain", "ok", 2u,
                                     p.req.method_len == 4u);
        (void)dx_http_response_error(out, sizeof(out), status != 0 ? status : 400, extra);
        (void)dx_http_request_find_header(&p.req, data, "Host", NULL, NULL);
    }

    /* Split feeding must agree with whole feeding. */
    if (size > 1u) {
        dx_http_parser_t q;
        dx_parse_result_t split;
        size_t half = size / 2u;
        size_t used = 0;

        dx_http_parser_init(&q, &limits);
        if (dx_http_parser_feed(&q, data, half, &used) == DX_PARSE_NEED_DATA) {
            split = dx_http_parser_feed(&q, data, size, &used);
            if (split != whole) {
                invariant_failed("split feeding disagrees with whole feeding");
            }
            if (whole == DX_PARSE_OK && used != consumed) {
                invariant_failed("split feeding consumed a different length");
            }
        }
    }
    return 0;
}

/* ----------------------- standalone driver ------------------------------ */

#ifdef FUZZ_STANDALONE

static uint32_t rng_state = 0x12345678u;

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
        "GET / HTTP/1.1\r\nHost: fuzz\r\n\r\n",
        "POST /a/b?c=d HTTP/1.0\r\nHost: h\r\nContent-Length: 0\r\n\r\n",
        "HEAD * HTTP/1.1\r\nHost: h\r\nX-A: 1\r\nX-B: 2\r\n\r\n",
        "OPTIONS / HTTP/1.1\r\nHost: h\r\n\r\n",
        "GET / HTTP/2.0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "\x01\x02\x03\r\n\r\n",
        "GET / HTTP/1.1\r\nBad Header: x\r\n\r\n",
        "GET /x\r\n\r\nHTTP/1.1\r\n\r\n",
    };
    static const size_t n_seeds = sizeof(seeds) / sizeof(seeds[0]);
    unsigned char buf[8192];
    const long iterations = 300000L;

    /* The fixed seed makes the CI run deterministic and reproducible. */
    rng_state = 0x12345678u;

    /* Every seed must pass through cleanly first. */
    for (size_t i = 0; i < n_seeds; i++) {
        LLVMFuzzerTestOneInput((const uint8_t *)seeds[i], strlen(seeds[i]));
    }

    for (long it = 0; it < iterations; it++) {
        size_t len;
        /* Start from a random seed and mutate. */
        {
            const char *seed = seeds[rng_next() % n_seeds];
            size_t seed_len = strlen(seed);

            len = (rng_next() % 1024u) + 1u;
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            memcpy(buf, seed, seed_len < len ? seed_len : len);
            if (seed_len < len) {
                memset(buf + seed_len, 'a', len - seed_len);
            }
        }
        /* Apply 1..8 random mutations. */
        {
            unsigned rounds = (unsigned)(rng_next() % 8u) + 1u;

            for (unsigned m = 0; m < rounds && len > 0u; m++) {
                size_t at = (size_t)(rng_next() % len);

                switch (rng_next() % 4u) {
                case 0: /* flip to a random byte, biased toward trouble */
                    buf[at] = (unsigned char)("\r\n \t:\x00\x7f\xff%"[rng_next() % 9u]);
                    break;
                case 1: /* truncate */
                    len = at + 1u;
                    break;
                case 2: /* duplicate a chunk (may hit the size cap) */
                    if (len + 64u < sizeof(buf)) {
                        memmove(buf + at + 32u, buf + at, len - at < 32u ? len - at : 32u);
                        len += 32u;
                    }
                    break;
                default: /* swap two bytes */
                    if (len > 1u) {
                        unsigned char t = buf[at];

                        buf[at] = buf[(at + 1u) % len];
                        buf[(at + 1u) % len] = t;
                    }
                    break;
                }
            }
        }
        LLVMFuzzerTestOneInput(buf, len);
    }

    printf("fuzz: %ld iterations, no invariant violations\n", iterations);
    return 0;
}

#endif /* FUZZ_STANDALONE */
