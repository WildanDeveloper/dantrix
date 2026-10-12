/*
 * status.h - Shared, lock-free server counters for the JSON status endpoint.
 *
 * Every field is a C11 _Atomic counter. Workers only increment them, so the
 * hot path stays lock-free: an atomic add on a per-worker cache line is far
 * cheaper than a mutex and never blocks. The status endpoint reads them with
 * relaxed ordering, which is enough for a monitoring snapshot.
 *
 * The counters live in one struct owned by the orchestrator and borrowed by
 * every worker and by the status renderer.
 */
#ifndef DX_STATUS_H
#define DX_STATUS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    _Atomic uint64_t active_connections; /* open right now */
    _Atomic uint64_t accepted_total;
    _Atomic uint64_t rejected_total;
    _Atomic uint64_t requests_total;
    _Atomic uint64_t resp_2xx;
    _Atomic uint64_t resp_3xx;
    _Atomic uint64_t resp_4xx;
    _Atomic uint64_t resp_5xx;
    _Atomic uint64_t bytes_sent_total;
} dx_status_t;

/* Zero every counter. Safe to call before the workers start. */
void dx_status_init(dx_status_t *st);

/* One connection was accepted / rejected / closed. */
void dx_status_conn_accepted(dx_status_t *st);
void dx_status_conn_rejected(dx_status_t *st);
void dx_status_conn_closed(dx_status_t *st);

/* A response with this status and body size was completed. */
void dx_status_response(dx_status_t *st, int status, uint64_t body_bytes);

/*
 * Render the JSON snapshot into out. Returns the number of bytes written, or 0
 * when the buffer is too small. version and uptime_seconds are printed as
 * given; workers is the configured worker count.
 */
size_t dx_status_json(const dx_status_t *st, char *out, size_t cap, const char *version,
                      uint64_t uptime_seconds, unsigned workers);

#endif /* DX_STATUS_H */
