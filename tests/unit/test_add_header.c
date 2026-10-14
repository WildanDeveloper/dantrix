/*
 * Unit tests for the `add_header` option: validation and emission.
 *
 * Two properties matter, and the first is a security one:
 *
 *   1. A configured header goes into the response verbatim, so a CR or LF in it
 *      would split the response -- the same attack the request parser refuses.
 *      Validation must reject that at configuration time, because a value that
 *      reaches the renderer has already been trusted.
 *   2. Every response carries the configured headers exactly once. Emitting them
 *      twice is not a crash but it is a protocol problem, and it happened: the
 *      first implementation added them in two places at once.
 */
#include "testing.h"

#include "config.h"
#include "conn.h"
#include "http_response.h"

#include <stdio.h>
#include <string.h>

static void test_validation(void)
{
    dx_test_begin("add_header/validation");

    /* Accepted: "Name: value", with the value allowed to contain a colon. */
    CHECK(dx_header_value_valid("X-Frame-Options: DENY"));
    CHECK(dx_header_value_valid("Referrer-Policy: no-referrer"));
    CHECK(dx_header_value_valid("X-A: b:c:d"));
    CHECK(dx_header_value_valid("X-Empty-Value: "));
    CHECK(dx_header_value_valid("X-Tab:\tvalue"));

    /* Rejected: no colon, empty name, space in the name, empty value string. */
    CHECK(!dx_header_value_valid("X-No-Colon"));
    CHECK(!dx_header_value_valid(": no name"));
    CHECK(!dx_header_value_valid("X Bad: v"));
    CHECK(!dx_header_value_valid(""));
    CHECK(!dx_header_value_valid(NULL));

    /* Rejected: CR, LF or NUL anywhere. This is the response-splitting case. */
    CHECK(!dx_header_value_valid("X-Inject: a\r\nX-Forged: yes"));
    CHECK(!dx_header_value_valid("X-Inject: a\rb"));
    CHECK(!dx_header_value_valid("X-Inject: a\nb"));
    CHECK(!dx_header_value_valid("X-Inject: a\r\n\r\nHTTP/1.1 200 OK"));
    CHECK(!dx_header_value_valid("X-Trailing: v\r\n"));
}

static void test_emission_once(void)
{
    static char headers[2][DX_CFG_STR_MAX];
    char out[2048];
    const char *needle = "X-Frame-Options: DENY";
    size_t len;
    int seen = 0;

    dx_test_begin("add_header/emitted_once");

    (void)snprintf(headers[0], sizeof(headers[0]), "X-Frame-Options: DENY");
    (void)snprintf(headers[1], sizeof(headers[1]), "Referrer-Policy: no-referrer");

    /* Off by default, which is what the unit tests and fuzzers rely on. */
    dx_http_response_set_extra_headers(NULL, 0u);
    len = dx_http_response_build_ex(out, sizeof(out), 200, NULL, "text/plain", "ok", 2u, false,
                                    "close", true);
    CHECK(len > 0u);
    CHECK(strstr(out, needle) == NULL);

    /* On: present exactly once, on a success response. */
    dx_http_response_set_extra_headers(headers, 2u);
    len = dx_http_response_build_ex(out, sizeof(out), 200, NULL, "text/plain", "ok", 2u, false,
                                    "close", true);
    CHECK(len > 0u);
    for (const char *p = out; (p = strstr(p, needle)) != NULL; p++) {
        seen++;
    }
    CHECK_EQ(seen, 1);
    CHECK(strstr(out, "Referrer-Policy: no-referrer") != NULL);

    /* And exactly once on an error response, through the other entry point. */
    seen = 0;
    len = dx_http_response_error(out, sizeof(out), 404, NULL);
    CHECK(len > 0u);
    for (const char *p = out; (p = strstr(p, needle)) != NULL; p++) {
        seen++;
    }
    CHECK_EQ(seen, 1);

    /* The caller's own headers and the configured ones coexist. */
    len = dx_http_response_build_ex(out, sizeof(out), 301, "Location: /a/\r\n", NULL, NULL, 0u,
                                    true, "close", true);
    CHECK(len > 0u);
    CHECK(strstr(out, "Location: /a/") != NULL);
    CHECK(strstr(out, needle) != NULL);

    /* Reset so a later test in this binary is not affected. */
    dx_http_response_set_extra_headers(NULL, 0u);
}

static void test_fit_within_the_staging_buffer(void)
{
    static char headers[DX_CFG_MAX_ADD_HEADERS][DX_CFG_STR_MAX];
    char out[DX_RESPONSE_TMP];
    size_t len;

    dx_test_begin("add_header/fits_the_staging_buffer");

    /*
     * The staging buffer is DX_RESPONSE_TMP, and dx_config_validate() refuses a
     * configuration whose headers cannot fit in it (that check exists because a
     * 14-header config was accepted and then made every response fail). This
     * test pins the other half: a configuration that PASSES validation must
     * produce a complete response, not a truncated one.
     */
    {
        /* The largest set that validation allows: fill until the budget is used
         * up, leaving DX_RESPONSE_HEADROOM for the rest of the response. */
        size_t budget = DX_RESPONSE_TMP - DX_RESPONSE_HEADROOM;
        size_t used = 0;
        size_t count = 0;

        while (count < DX_CFG_MAX_ADD_HEADERS) {
            size_t room = budget - used;

            if (room < 20u) {
                break;
            }
            int n = snprintf(headers[count], sizeof(headers[count]), "X-Pad-%zu: ", count);

            /* Fill the value up to the room available, minus the CRLF. */
            {
                size_t value_len = room - (size_t)n - 2u;

                if (value_len > sizeof(headers[count]) - (size_t)n - 1u) {
                    value_len = sizeof(headers[count]) - (size_t)n - 1u;
                }
                for (size_t k = (size_t)n; k < (size_t)n + value_len; k++) {
                    headers[count][k] = 'p';
                }
                headers[count][(size_t)n + value_len] = '\0';
            }
            used += strlen(headers[count]) + 2u;
            count++;
        }
        CHECK(count > 0u);
        dx_http_response_set_extra_headers(headers, count);
        len = dx_http_response_build_ex(out, sizeof(out), 200, NULL, "text/plain", "ok", 2u, false,
                                        "close", true);
        /*
         * A complete response. It ends with the body ("ok"), not with a newline,
         * so the check is that the header block is terminated AND the body is
         * present -- an earlier version of this assertion looked for a trailing
         * '\n' and failed on a correct response.
         */
        CHECK(len > 0u);
        CHECK(strstr(out, "X-Pad-0: ") != NULL);
        CHECK(strstr(out, "\r\n\r\nok") != NULL);
    }

    dx_http_response_set_extra_headers(NULL, 0u);
}

int main(void)
{
    test_validation();
    test_emission_once();
    test_fit_within_the_staging_buffer();
    printf("test_add_header: ");
    return dx_test_summary();
}
