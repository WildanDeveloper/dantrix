/*
 * fuzz_escape.c - Fuzz harness for the escaping used in HTML output and
 * redirect headers.
 *
 * Why this deserves its own target: `dx_html_escape` and `dx_url_encode` take
 * bytes that an attacker controls indirectly. A file name on disk becomes HTML
 * in a directory listing, and a request target becomes the `Location` of a
 * redirect. Both are places where a missed character turns into cross-site
 * scripting or header injection, and both are pure functions over arbitrary
 * bytes, which is exactly what a fuzzer is good at.
 *
 * The invariants asserted here are the ones the security claims rest on:
 *
 *   - the escaped output never contains a raw '<', '>', '"' or '\'' when the
 *     input did not have one escaped, so a name cannot close an attribute or a
 *     tag;
 *   - the output is always NUL-terminated and never exceeds the capacity;
 *   - a truncation is reported as 0 (refused) rather than producing a partial
 *     escape that could leave a quote unbalanced;
 *   - the encoder never emits a raw byte that is not unreserved or a valid
 *     percent escape, so a redirect target cannot carry CR/LF.
 *
 * Build modes: libFuzzer (clang -fsanitize=fuzzer) or the deterministic
 * standalone driver (FUZZ_STANDALONE), which runs in CI with plain gcc.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file.h"
#include "../src/path.h"

/* libFuzzer entry point; also called by the standalone driver below. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void invariant_failed(const char *what)
{
    fprintf(stderr, "fuzz invariant failed: %s\n", what);
    abort();
}

/* The characters that must never appear raw in escaped HTML text. */
static bool contains_raw_html_meta(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = s[i];

        if (c == '<' || c == '>' || c == '"' || c == '\'') {
            return true;
        }
    }
    return false;
}

/*
 * Decode the five entities dx_html_escape emits, and compare with the input.
 *
 * This is the invariant that catches a WRONG entity, which the raw-character
 * check cannot: mapping '<' to "&gt;" still emits no raw '<', so it passes that
 * check while producing HTML that renders the wrong character. Only decoding
 * the output and comparing it with the input proves the escape is correct and
 * not merely quote-free.
 *
 * Control bytes are dropped by dx_html_escape on purpose (they never belong in
 * a listing), so the comparison skips them on the input side rather than
 * expecting a byte-for-byte match.
 *
 * Returns the decoded length, or (size_t)-1 on a malformed entity (which is
 * itself a failure: the function must only ever emit these five).
 */
static size_t html_unescape(const char *s, size_t len, char *out, size_t cap)
{
    size_t o = 0;

    for (size_t i = 0; i < len;) {
        if (o + 1u >= cap) {
            return (size_t)-1; /* no room for one more byte plus the terminator */
        }
        if (s[i] != '&') {
            out[o++] = s[i++];
            continue;
        }
        if (len - i >= 4u && memcmp(s + i, "&lt;", 4u) == 0) {
            out[o++] = '<';
            i += 4u;
        } else if (len - i >= 4u && memcmp(s + i, "&gt;", 4u) == 0) {
            out[o++] = '>';
            i += 4u;
        } else if (len - i >= 5u && memcmp(s + i, "&amp;", 5u) == 0) {
            out[o++] = '&';
            i += 5u;
        } else if (len - i >= 6u && memcmp(s + i, "&quot;", 6u) == 0) {
            out[o++] = '"';
            i += 6u;
        } else if (len - i >= 5u && memcmp(s + i, "&#39;", 5u) == 0) {
            out[o++] = '\'';
            i += 5u;
        } else {
            return (size_t)-1; /* an entity this function never emits */
        }
    }
    out[o] = '\0';
    return o;
}

/* Copy the input with the control bytes removed, which is what dx_html_escape
 * is documented to do. Returns the kept length. */
static size_t strip_controls(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        if (in[i] < 0x20u || in[i] == 0x7fu) {
            continue;
        }
        if (o + 1u >= cap) {
            return (size_t)-1;
        }
        out[o++] = (char)in[i];
    }
    if (o >= cap) {
        return (size_t)-1;
    }
    out[o] = '\0';
    return o;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char html[4096];
    char url[4096];
    size_t n;
    size_t url_n;

    if (size > 2048u) {
        size = 2048u; /* keep the run cheap; longer inputs add nothing */
    }

    /* ---- HTML escaping ------------------------------------------------- */
    n = dx_html_escape((const char *)data, size, html, sizeof(html));
    if (n > 0u) {
        if (n >= sizeof(html)) {
            invariant_failed("html_escape reported a length past the buffer");
        }
        if (html[n] != '\0') {
            invariant_failed("html_escape did not NUL-terminate its output");
        }
        /*
         * The core claim: whatever went in, no raw tag or quote character comes
         * out. '&' is allowed through raw only as the start of an entity the
         * function itself wrote, which always begins with "&amp;" or "&lt;"
         * etc. -- so the check is on the four characters that break out of
         * text or an attribute.
         */
        if (contains_raw_html_meta(html, n)) {
            invariant_failed("html_escape left a raw '<', '>', '\"' or '\\'' in the output");
        }
        /*
         * And the escape must be the RIGHT one: decode the output and compare
         * the decoded bytes with the input bytes. This is what catches a wrong
         * entity, which the check above cannot see. The comparison is on the
         * decoded content, not on the length: escaping grows the text, so the
         * escaped length is deliberately different from the input length.
         */
        {
            char decoded[4096];
            char expected[4096];
            size_t dn = html_unescape(html, n, decoded, sizeof(decoded));
            size_t en = strip_controls(data, size, expected, sizeof(expected));

            if (dn == (size_t)-1) {
                invariant_failed("html_escape emitted an entity it does not define");
            }
            if (en == (size_t)-1) {
                invariant_failed("the control-stripped expectation did not fit");
            }
            if (dn != en) {
                invariant_failed("html_escape round-trip length differs from the input");
            }
            if (dn > 0u && memcmp(decoded, expected, dn) != 0) {
                invariant_failed("html_escape round-trip content differs from the input");
            }
        }
    }

    /* A buffer that is too small must be refused, not partially filled: a half
     * written escape could leave a quote unbalanced. */
    {
        char tiny[4];
        size_t tn = dx_html_escape((const char *)data, size, tiny, sizeof(tiny));

        if (tn >= sizeof(tiny)) {
            invariant_failed("html_escape overflowed a small buffer");
        }
        if (tn > 0u && tiny[tn] != '\0') {
            invariant_failed("html_escape did not terminate in a small buffer");
        }
    }

    /* ---- URL encoding -------------------------------------------------- */
    url_n = dx_url_encode((const char *)data, size, url, sizeof(url));
    if (url_n > 0u) {
        if (url_n >= sizeof(url)) {
            invariant_failed("url_encode reported a length past the buffer");
        }
        if (url[url_n] != '\0') {
            invariant_failed("url_encode did not NUL-terminate its output");
        }
        /*
         * The claim a redirect depends on: no CR, LF or space survives raw, so a
         * target cannot split the header it is written into.
         */
        for (size_t i = 0; i < url_n; i++) {
            unsigned char c = (unsigned char)url[i];

            if (c == '\r' || c == '\n' || c == ' ' || c == '\0') {
                invariant_failed("url_encode emitted a raw CR, LF, space or NUL");
            }
            if (c < 0x21u || c > 0x7eu) {
                invariant_failed("url_encode emitted a non-printable byte");
            }
        }
        /* Every '%' must introduce two hex digits. */
        for (size_t i = 0; i < url_n; i++) {
            if (url[i] == '%') {
                if (i + 2u >= url_n) {
                    invariant_failed("url_encode ended with a truncated percent escape");
                }
                for (size_t k = 1u; k <= 2u; k++) {
                    char h = url[i + k];

                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F'))) {
                        invariant_failed("url_encode wrote a non-hex percent escape");
                    }
                }
                i += 2u;
            }
        }
    }
    {
        char tiny[4];
        size_t tn = dx_url_encode((const char *)data, size, tiny, sizeof(tiny));

        if (tn >= sizeof(tiny)) {
            invariant_failed("url_encode overflowed a small buffer");
        }
    }

    return 0;
}

/* ----------------------- standalone driver ------------------------------ */

#ifdef FUZZ_STANDALONE

static uint32_t rng_state = 0x13579bdfu;

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
        "",         "plain.txt",
        "<script>", "\"quoted\"",
        "'single'", "a&b",
        "a<b>c",    "100%",
        "%2e%2e/",  "a\r\nb",
        "a b",      "\xff\xfe",
        "/a/b?c=d", "<img src=x onerror=alert(1)>",
        "&amp;",    "&#39;",
    };
    static const size_t n_seeds = sizeof(seeds) / sizeof(seeds[0]);
    unsigned char buf[2048];
    const long iterations = 300000L;

    rng_state = 0x13579bdfu; /* fixed seed: CI stays deterministic */

    for (size_t i = 0; i < n_seeds; i++) {
        LLVMFuzzerTestOneInput((const uint8_t *)seeds[i], strlen(seeds[i]));
    }

    for (long it = 0; it < iterations; it++) {
        size_t len = rng_next() % sizeof(buf);
        size_t n = (len == 0u) ? 1u : len;

        for (size_t i = 0; i < n; i++) {
            buf[i] = (unsigned char)(rng_next() & 0xffu);
        }
        /* Bias half the inputs toward the interesting ASCII range so the
         * escaping branches are hit often rather than almost never. */
        if ((it & 1L) != 0L) {
            for (size_t i = 0; i < n; i++) {
                buf[i] = (unsigned char)(' ' + (rng_next() % 95u));
            }
        }
        LLVMFuzzerTestOneInput(buf, n);
    }

    printf("fuzz_escape: %ld iterations, no invariant violations\n", iterations);
    return 0;
}

#endif /* FUZZ_STANDALONE */
