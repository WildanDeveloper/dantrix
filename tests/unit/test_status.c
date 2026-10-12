/* Unit tests for src/status.c (atomic counters and the JSON snapshot). */
#include "testing.h"

#include "status.h"

#include <string.h>

static void test_counters(void)
{
    dx_status_t st;

    dx_test_begin("status/counters");

    dx_status_init(&st);

    dx_status_conn_accepted(&st);
    dx_status_conn_accepted(&st);
    dx_status_conn_closed(&st);
    dx_status_conn_rejected(&st);
    dx_status_response(&st, 200, 100);
    dx_status_response(&st, 301, 0);
    dx_status_response(&st, 404, 12);
    dx_status_response(&st, 503, 5);

    CHECK_EQ((long long)atomic_load(&st.accepted_total), 2);
    CHECK_EQ((long long)atomic_load(&st.active_connections), 1);
    CHECK_EQ((long long)atomic_load(&st.rejected_total), 1);
    CHECK_EQ((long long)atomic_load(&st.requests_total), 4);
    CHECK_EQ((long long)atomic_load(&st.resp_2xx), 1);
    CHECK_EQ((long long)atomic_load(&st.resp_3xx), 1);
    CHECK_EQ((long long)atomic_load(&st.resp_4xx), 1);
    CHECK_EQ((long long)atomic_load(&st.resp_5xx), 1);
    CHECK_EQ((long long)atomic_load(&st.bytes_sent_total), 117);

    /* NULL is a no-op, never a crash. */
    dx_status_init(NULL);
    dx_status_conn_accepted(NULL);
    dx_status_conn_closed(NULL);
    dx_status_conn_rejected(NULL);
    dx_status_response(NULL, 200, 1);

    /*
     * A close must never push the unsigned counter below zero: clamp at zero
     * instead of wrapping to a huge value.
     */
    dx_status_init(&st);
    dx_status_conn_closed(&st);
    dx_status_conn_closed(&st);
    dx_status_conn_closed(&st);
    CHECK_EQ((long long)atomic_load(&st.active_connections), 0);
    dx_status_conn_accepted(&st);
    dx_status_conn_accepted(&st);
    dx_status_conn_closed(&st);
    CHECK_EQ((long long)atomic_load(&st.active_connections), 1);
}

static void test_json(void)
{
    dx_status_t st;
    char buf[1024];
    size_t n;

    dx_test_begin("status/json");

    dx_status_init(&st);
    dx_status_conn_accepted(&st);
    dx_status_response(&st, 200, 42);

    n = dx_status_json(&st, buf, sizeof(buf), "9.9.9", 123, 4);
    CHECK(n > 0);
    CHECK(strstr(buf, "\"version\": \"9.9.9\"") != NULL);
    CHECK(strstr(buf, "\"uptime_seconds\": 123") != NULL);
    CHECK(strstr(buf, "\"workers\": 4") != NULL);
    CHECK(strstr(buf, "\"active\": 1") != NULL);
    CHECK(strstr(buf, "\"accepted_total\": 1") != NULL);
    CHECK(strstr(buf, "\"2xx\": 1") != NULL);
    CHECK(strstr(buf, "\"bytes_sent_total\": 42") != NULL);

    /* The buffer ends with a newline and the length matches strlen. */
    CHECK_EQ((long long)n, (long long)strlen(buf));
    CHECK(n > 0u);
    CHECK(buf[n - 1] == '\n');

    /* A buffer that is too small is refused, not truncated. */
    CHECK_EQ(dx_status_json(&st, buf, 8u, "x", 0, 1), 0u);
    CHECK_EQ(dx_status_json(NULL, buf, sizeof(buf), "x", 0, 1), 0u);
    CHECK_EQ(dx_status_json(&st, NULL, sizeof(buf), "x", 0, 1), 0u);
}

int main(void)
{
    test_counters();
    test_json();
    printf("test_status: ");
    return dx_test_summary();
}
