#include "status.h"

#include <stdio.h>

void dx_status_init(dx_status_t *st)
{
    if (st == NULL) {
        return;
    }
    atomic_store(&st->active_connections, 0);
    atomic_store(&st->accepted_total, 0);
    atomic_store(&st->rejected_total, 0);
    atomic_store(&st->requests_total, 0);
    atomic_store(&st->resp_2xx, 0);
    atomic_store(&st->resp_3xx, 0);
    atomic_store(&st->resp_4xx, 0);
    atomic_store(&st->resp_5xx, 0);
    atomic_store(&st->bytes_sent_total, 0);
}

void dx_status_conn_accepted(dx_status_t *st)
{
    if (st != NULL) {
        atomic_fetch_add_explicit(&st->accepted_total, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st->active_connections, 1, memory_order_relaxed);
    }
}

void dx_status_conn_rejected(dx_status_t *st)
{
    if (st != NULL) {
        atomic_fetch_add_explicit(&st->rejected_total, 1, memory_order_relaxed);
    }
}

void dx_status_conn_closed(dx_status_t *st)
{
    if (st == NULL) {
        return;
    }
    /*
     * The counter is unsigned, so a close that is ever not paired with an
     * accept would wrap it to a huge value and poison the metrics. Clamp at
     * zero with a compare-exchange instead of a blind decrement.
     */
    {
        uint64_t cur = atomic_load_explicit(&st->active_connections, memory_order_relaxed);

        while (cur > 0) {
            if (atomic_compare_exchange_weak_explicit(&st->active_connections, &cur, cur - 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                return;
            }
        }
    }
}

void dx_status_response(dx_status_t *st, int status, uint64_t body_bytes)
{
    if (st == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&st->requests_total, 1, memory_order_relaxed);
    if (status >= 200 && status < 300) {
        atomic_fetch_add_explicit(&st->resp_2xx, 1, memory_order_relaxed);
    } else if (status >= 300 && status < 400) {
        atomic_fetch_add_explicit(&st->resp_3xx, 1, memory_order_relaxed);
    } else if (status >= 400 && status < 500) {
        atomic_fetch_add_explicit(&st->resp_4xx, 1, memory_order_relaxed);
    } else if (status >= 500) {
        atomic_fetch_add_explicit(&st->resp_5xx, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&st->bytes_sent_total, body_bytes, memory_order_relaxed);
}

size_t dx_status_json(const dx_status_t *st, char *out, size_t cap, const char *version,
                      uint64_t uptime_seconds, unsigned workers)
{
    uint64_t active;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t requests;
    uint64_t r2;
    uint64_t r3;
    uint64_t r4;
    uint64_t r5;
    uint64_t sent;
    int n;

    if (st == NULL || out == NULL || cap == 0) {
        return 0;
    }
    /* Relaxed reads: a monitoring snapshot does not need a consistent view. */
    active = atomic_load_explicit(&st->active_connections, memory_order_relaxed);
    accepted = atomic_load_explicit(&st->accepted_total, memory_order_relaxed);
    rejected = atomic_load_explicit(&st->rejected_total, memory_order_relaxed);
    requests = atomic_load_explicit(&st->requests_total, memory_order_relaxed);
    r2 = atomic_load_explicit(&st->resp_2xx, memory_order_relaxed);
    r3 = atomic_load_explicit(&st->resp_3xx, memory_order_relaxed);
    r4 = atomic_load_explicit(&st->resp_4xx, memory_order_relaxed);
    r5 = atomic_load_explicit(&st->resp_5xx, memory_order_relaxed);
    sent = atomic_load_explicit(&st->bytes_sent_total, memory_order_relaxed);

    n = snprintf(out, cap,
                 "{\n"
                 "  \"version\": \"%s\",\n"
                 "  \"uptime_seconds\": %llu,\n"
                 "  \"workers\": %u,\n"
                 "  \"connections\": {\n"
                 "    \"active\": %llu,\n"
                 "    \"accepted_total\": %llu,\n"
                 "    \"rejected_total\": %llu\n"
                 "  },\n"
                 "  \"requests_total\": %llu,\n"
                 "  \"responses\": {\n"
                 "    \"2xx\": %llu,\n"
                 "    \"3xx\": %llu,\n"
                 "    \"4xx\": %llu,\n"
                 "    \"5xx\": %llu\n"
                 "  },\n"
                 "  \"bytes_sent_total\": %llu\n"
                 "}\n",
                 (version != NULL) ? version : "", (unsigned long long)uptime_seconds, workers,
                 (unsigned long long)active, (unsigned long long)accepted,
                 (unsigned long long)rejected, (unsigned long long)requests, (unsigned long long)r2,
                 (unsigned long long)r3, (unsigned long long)r4, (unsigned long long)r5,
                 (unsigned long long)sent);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}
