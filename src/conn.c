#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dantrix.h"
#include "file.h"
#include "http_range.h"
#include "http_response.h"
#include "mime.h"
#include "net.h"
#include "path.h"
#include "status.h"
#include "util.h"
#include "worker.h"

/* --------------------------------------------------------------------- */
/* Slot pool                                                              */
/* --------------------------------------------------------------------- */

/*
 * Slot memory lives in chunks so that a chunk is never moved once allocated.
 * Chunk base pointers are kept in a separate index array, which makes slot
 * lookup a plain division and keeps existing dx_conn_t pointers stable.
 */
typedef struct {
    dx_conn_t *mem;
} dx_chunk_t;

struct dx_pool {
    dx_chunk_t **chunks; /* base pointers, grows by doubling */
    size_t nchunks;
    size_t chunks_cap;
    size_t conn_slots;   /* slots per chunk */
    size_t max_conns;    /* hard cap of simultaneously held slots */
    uint32_t *free_list; /* slot indices ready for reuse */
    size_t free_count;
    size_t free_cap;
    size_t total_slots;
    size_t active;
    uint64_t next_id;
};

dx_pool_t *dx_pool_create(size_t conn_slots, size_t max_conns)
{
    dx_pool_t *pool;

    if (conn_slots == 0 || max_conns == 0) {
        return NULL;
    }

    pool = (dx_pool_t *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->conn_slots = conn_slots;
    pool->max_conns = max_conns;
    return pool;
}

void dx_pool_destroy(dx_pool_t *pool)
{
    size_t i;

    if (pool == NULL) {
        return;
    }
    for (i = 0; i < pool->nchunks; i++) {
        free(pool->chunks[i]->mem);
        free(pool->chunks[i]);
    }
    free(pool->chunks);
    free(pool->free_list);
    free(pool);
}

/*
 * Reserve room for n more entries before any chunk memory is allocated; false
 * when memory runs out. Growing first guarantees that a successfully allocated
 * chunk always lands in the free list in full.
 */
static bool pool_free_list_reserve(struct dx_pool *pool, size_t n)
{
    size_t needed = pool->free_count + n;
    size_t new_cap;

    if (needed <= pool->free_cap) {
        return true;
    }
    new_cap = (pool->free_cap == 0) ? 64u : pool->free_cap;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2u) {
            return false; /* doubling would overflow */
        }
        new_cap *= 2u;
    }
    {
        uint32_t *p = (uint32_t *)realloc(pool->free_list, new_cap * sizeof(*p));

        if (p == NULL) {
            return false;
        }
        pool->free_list = p;
        pool->free_cap = new_cap;
    }
    return true;
}

/* Allocate one chunk of conn slots and record it. Returns false on failure. */
static bool pool_add_chunk(dx_pool_t *pool)
{
    dx_chunk_t *chunk;
    dx_conn_t *mem;
    dx_chunk_t **new_chunks;
    size_t i;

    if (pool->total_slots >= 0xffffffffu) {
        return false; /* the tag index space is 32 bits */
    }

    mem = (dx_conn_t *)calloc(pool->conn_slots, sizeof(dx_conn_t));
    if (mem == NULL) {
        return false;
    }
    chunk = (dx_chunk_t *)calloc(1, sizeof(*chunk));
    if (chunk == NULL) {
        free(mem);
        return false;
    }
    chunk->mem = mem;

    if (pool->nchunks == pool->chunks_cap) {
        size_t new_cap = (pool->chunks_cap == 0) ? 4u : pool->chunks_cap * 2u;

        new_chunks = (dx_chunk_t **)realloc(pool->chunks, new_cap * sizeof(*new_chunks));
        if (new_chunks == NULL) {
            free(chunk);
            free(mem);
            return false;
        }
        pool->chunks = new_chunks;
        pool->chunks_cap = new_cap;
    }

    /* Free list capacity is guaranteed before the chunk becomes visible. */
    if (!pool_free_list_reserve(pool, pool->conn_slots)) {
        free(chunk);
        free(mem);
        return false;
    }

    pool->chunks[pool->nchunks++] = chunk;

    for (i = 0; i < pool->conn_slots; i++) {
        size_t global_index = pool->total_slots + i;

        if (global_index > 0xffffffffu) {
            break; /* unreachable: guarded by the 32 bit check above */
        }
        pool->free_list[pool->free_count++] = (uint32_t)global_index;
    }
    pool->total_slots += pool->conn_slots;
    return true;
}

dx_conn_t *dx_pool_acquire(dx_pool_t *pool)
{
    uint32_t index;
    dx_conn_t *conn;

    if (pool == NULL) {
        return NULL;
    }
    if (pool->active >= pool->max_conns) {
        return NULL; /* caller decides whether to log; this is a common event */
    }
    if (pool->free_count == 0 && !pool_add_chunk(pool)) {
        return NULL;
    }

    index = pool->free_list[--pool->free_count];
    conn = dx_pool_slot(pool, (size_t)index);
    if (conn == NULL) {
        /* Should be unreachable; put the index back and report exhaustion. */
        if (pool->free_count < pool->free_cap) {
            pool->free_list[pool->free_count++] = index;
        }
        return NULL;
    }

    /*
     * Reset every field except the generation, never memset: the generation
     * must survive the recycle so that events stamped with the previous one are
     * recognised as stale instead of resolving to this new owner.
     *
     * dx_conn_reset() owns the full list and frees every allocation, so a slot
     * can never carry a stale pointer, counter or access record into its next
     * life. It keeps the generation and the index untouched.
     */
    conn->index = index; /* constant per slot; re-set defensively */
    dx_conn_reset(conn);
    conn->state = DX_CONN_READING;

    /*
     * Skip generation 0 so a zeroed tag can never look valid, and wrap at the
     * width the tag actually carries. dx_tag_conn() masks the generation with
     * DX_TAG_GEN_MASK before storing it, while dx_pool_lookup() compares the
     * stored value with the full one; letting the counter grow past the mask
     * would make every event for that slot look stale after 2^30 recycles.
     */
    if (conn->generation >= DX_TAG_GEN_MASK) {
        conn->generation = 0u;
    }
    conn->generation++;
    conn->id = ++pool->next_id;
    pool->active++;
    return conn;
}

/*
 * Return a slot. dx_conn_reset() runs first so the invariant is enforced here
 * instead of relying on every caller to remember: a slot handed out again must
 * never carry the previous owner's state or pointers, no matter which path
 * released it. The caller must have closed the descriptor and returned the
 * pooled read buffer beforehand; the heap allocations the slot owns are freed
 * here.
 */
bool dx_pool_release(dx_pool_t *pool, dx_conn_t *conn)
{
    if (pool == NULL || conn == NULL) {
        return false;
    }
    if (conn->state == DX_CONN_FREE) {
        return false; /* double release */
    }

    /*
     * Clean the slot before it becomes reusable. The caller has already closed
     * the descriptor and returned the pooled read buffer, so everything that is
     * left here is owned by the slot itself and is freed here.
     */
    dx_conn_reset(conn);
    conn->fd = -1;
    if (pool->active > 0) {
        pool->active--;
    }
    /*
     * Capacity is guaranteed for every live slot (see pool_free_list_reserve),
     * so this push cannot fail for a properly owned slot.
     */
    if (pool->free_count < pool->free_cap) {
        pool->free_list[pool->free_count++] = conn->index;
    }
    return true;
}

dx_conn_t *dx_pool_slot(dx_pool_t *pool, size_t index)
{
    size_t ordinal;
    size_t slot;

    if (pool == NULL || index >= pool->total_slots) {
        return NULL;
    }
    ordinal = index / pool->conn_slots;
    slot = index % pool->conn_slots;
    if (ordinal >= pool->nchunks) {
        return NULL;
    }
    return &pool->chunks[ordinal]->mem[slot];
}

dx_conn_t *dx_pool_lookup(dx_pool_t *pool, uint32_t index, uint32_t generation)
{
    dx_conn_t *conn = dx_pool_slot(pool, (size_t)index);

    if (conn == NULL || conn->state == DX_CONN_FREE) {
        return NULL;
    }
    if (conn->generation != generation) {
        return NULL; /* the slot was recycled: the event is stale */
    }
    return conn;
}

size_t dx_pool_total_slots(const dx_pool_t *pool)
{
    return (pool == NULL) ? 0u : pool->total_slots;
}

size_t dx_pool_active(const dx_pool_t *pool)
{
    return (pool == NULL) ? 0u : pool->active;
}

/* --------------------------------------------------------------------- */
/* Per-connection HTTP flow                                               */
/* --------------------------------------------------------------------- */

/* Stack staging area for one response; 2 KiB covers every v0.2 response.
 * File responses append validators on top, so the buffer is sized for that. */

/* Scratch for the pread()+send() fallback of sendfile(). */
#define DX_FILE_SCRATCH 16384u

/* Buffer for a generated directory listing. */
#define DX_LISTING_TMP 32768u

/* The single body v0.2 serves when no document root is configured. */
static const char conn_hello_body[] = "Dantrix " DX_VERSION_STRING ": the HTTP core is alive.\n";

/* Forward declarations: the keep-alive helpers are shared by the static and
 * the in-memory response paths, which live in different order in this file. */
static int conn_after_response(struct dx_worker *w, dx_conn_t *conn);
static int conn_flush_mem_body(struct dx_worker *w, dx_conn_t *conn);
static int conn_grow_to(struct dx_worker *w, dx_conn_t *conn, size_t need);

/*
 * Bytes of the current request that still sit in the read buffer and must be
 * dropped before the response is staged.
 *
 * Normal case: the header block and the body are both still in the buffer. When
 * the body was drained incrementally the header block sits in conn->hdr and
 * every body byte was already discarded as it was counted, so nothing of this
 * request is left in the buffer: whatever is in there belongs to the next
 * pipelined request and must be preserved.
 */
static size_t conn_request_bytes(const dx_conn_t *conn)
{
    if (conn->hdr != NULL) {
        return 0;
    }
    return conn->body_offset + (size_t)conn->parser.req.content_length;
}
static bool response_keeps_alive(const struct dx_worker *w, const dx_conn_t *conn,
                                 const dx_http_request_t *req, const unsigned char *base,
                                 int status);
static const char *connection_value(bool keep_alive, int minor_version);
static int conn_hold_pipelined(dx_conn_t *conn, size_t leftover);

void dx_conn_init(dx_conn_t *conn, int fd, const dx_http_limits_t *limits)
{
    if (conn == NULL) {
        return;
    }
    conn->fd = fd;
    conn->state = DX_CONN_READING;
    conn->eof = false;
    conn->close_conn = false;
    conn->buf_mem = NULL;
    conn->hold = NULL;
    conn->hold_len = 0;
    conn->file_fd = -1;
    conn->file_offset = 0;
    conn->file_left = 0;
    conn->body_left = 0;
    conn->requests = 0;
    conn->hdr_table = NULL;
    conn->hdr_mem = NULL;
    dx_timer_node_reset(&conn->timer);
    dx_buffer_init(&conn->buf, NULL, 0);
    /* No header table yet: it is acquired with the read buffer on first read. */
    dx_http_parser_init_ex(&conn->parser, limits, NULL, 0u);
    memset(&conn->acc, 0, sizeof(conn->acc));
    conn->peer[0] = '-';
    conn->peer[1] = '\0';
    conn->bytes_in = 0;
    conn->bytes_out = 0;
}

void dx_conn_release_file(dx_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    if (conn->file_fd >= 0) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
    }
    conn->file_offset = 0;
    conn->file_left = 0;
}

/*
 * Release the owned scratch allocations: the in-memory response body and the
 * header block saved while a request body was drained. Both are plain heap
 * blocks, so this is safe on a slot that never used them.
 */
void dx_conn_release_scratch(dx_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    free(conn->mem_body);
    conn->mem_body = NULL;
    conn->mem_body_len = 0;
    conn->mem_body_off = 0;
    free(conn->hdr);
    conn->hdr = NULL;
    conn->hdr_len = 0;
}

/* Release pipelined bytes parked while a response was staged. */
void dx_conn_release_hold(dx_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    if (conn->hold != NULL) {
        free(conn->hold);
        conn->hold = NULL;
    }
    conn->hold_len = 0;
}

void dx_conn_record_status(dx_conn_t *conn, int status)
{
    if (conn == NULL) {
        return;
    }
    conn->acc.status = status;
    conn->acc.body_bytes = 0;
}

void dx_conn_release_buffer(dx_conn_t *conn, dx_bufpool_t *pool)
{
    if (conn == NULL) {
        return;
    }
    if (conn->buf_mem != NULL) {
        (void)dx_bufpool_release(pool, conn->buf_mem);
        conn->buf_mem = NULL;
    } else if (conn->buf.data != NULL) {
        /*
         * The buffer outgrew its pooled slot at some point and lives on the
         * heap; hand it back to the allocator instead of leaking it.
         */
        free(conn->buf.data);
    }
    dx_buffer_detach(&conn->buf);
}

/*
 * Put a slot back into its pristine state.
 *
 * Every allocation the slot owns is freed or cleared here, so a recycled slot
 * can never carry the previous owner's memory or counters into its next life.
 * Two leaks lived exactly here: a read buffer that outgrew its pooled slot was
 * detached but never freed, and the saved header block and in-memory body
 * were never released at all.
 *
 * The generation is deliberately left alone: it must survive the recycle so
 * events stamped with the previous one are recognised as stale.
 */
void dx_conn_reset(dx_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    conn->fd = -1;
    conn->state = DX_CONN_FREE;
    conn->eof = false;
    conn->close_conn = false;
    conn->response_pending = false;

    /*
     * The read buffer is the pool slot recorded in buf_mem, and that slot
     * belongs to the buffer pool: it is never freed here (it is not even a
     * heap pointer of its own). Only a buffer that outgrew its slot and was
     * replaced by malloc() memory is ours to release, which is exactly the
     * case buf_mem == NULL marks.
     */
    if (conn->buf_mem == NULL && conn->buf.data != NULL) {
        free(conn->buf.data);
    }
    conn->buf_mem = NULL;
    dx_buffer_detach(&conn->buf);

    free(conn->hold);
    conn->hold = NULL;
    conn->hold_len = 0;

    free(conn->mem_body);
    conn->mem_body = NULL;
    conn->mem_body_len = 0;
    conn->mem_body_off = 0;

    free(conn->hdr);
    conn->hdr = NULL;
    conn->hdr_len = 0;

    /*
     * The pooled header table belongs to the header pool, not to the slot: the
     * caller returns it (dx_conn_release_headers). Resetting only clears the
     * pointer so a recycled slot can never hand a stale table to the parser.
     */
    conn->hdr_table = NULL;
    conn->hdr_mem = NULL;

    /*
     * The streamed file belongs to the connection lifecycle and is closed by
     * dx_conn_release_file() before the slot is released. Resetting must only
     * clear the field: closing it here would close whatever descriptor number
     * the stale value happens to be, and the kernel happily hands that number
     * straight back to the next connection.
     */
    conn->file_fd = -1;
    conn->file_offset = 0;
    conn->file_left = 0;

    conn->body_left = 0;
    conn->body_offset = 0;
    conn->requests = 0;
    conn->bytes_in = 0;
    conn->bytes_out = 0;
    memset(&conn->acc, 0, sizeof(conn->acc));
    dx_buffer_init(&conn->buf, NULL, 0);
}

bool dx_conn_has_pending(const dx_conn_t *conn)
{
    return (conn != NULL) && dx_buffer_readable(&conn->buf) > 0;
}

/*
 * True while the server still owes the client bytes: a staged response that
 * is not fully written, a file body in flight, or the tail of an in-memory
 * body. Request bytes sitting in the same buffer do not count: they are
 * input, not output.
 */
bool dx_conn_has_work(const dx_conn_t *conn)
{
    if (conn == NULL) {
        return false;
    }
    return conn->response_pending || conn->file_left > 0 || conn->mem_body_off < conn->mem_body_len;
}

/*
 * Try to send everything pending. On EAGAIN the state moves to WRITING so the
 * worker registers EPOLLOUT. A full drain returns -1 when the connection must
 * close right away: the response already decided that (close_conn), or the
 * peer sent EOF and there is nothing left to wait for.
 */
static int conn_flush(struct dx_worker *w, dx_conn_t *conn)
{
    while (dx_buffer_readable(&conn->buf) > 0) {
        size_t want = dx_buffer_readable(&conn->buf);
        ssize_t sent = send(conn->fd, conn->buf.data + conn->buf.off, want, MSG_NOSIGNAL);

        if (sent > 0) {
            size_t n = (size_t)sent;

            conn->bytes_out += n;
            dx_buffer_consume(&conn->buf, n);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn->state = DX_CONN_WRITING;
            conn->response_pending = true;
            return 0;
        }
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: send: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }
    conn->state = DX_CONN_READING;
    conn->response_pending = false;
    return 0;
}

/*
 * Stream the file body. sendfile() moves page cache pages straight into the
 * socket; when it is disabled, or fails with EINVAL/ENOSYS on the filesystem,
 * the pread()+send() fallback is used. The per call limit keeps one large
 * transfer from starving other connections.
 */
static int conn_flush_file(struct dx_worker *w, dx_conn_t *conn)
{
    unsigned char scratch[DX_FILE_SCRATCH];

    /*
     * sendfile() writes straight to the socket, so it must not run while the
     * staged response headers are still partly in the read buffer: the body
     * would overtake the rest of the header and break the framing. The
     * pread()+send() path is safe here because it parks bytes behind the
     * header in the same buffer, so the guard only applies to sendfile.
     */
    if (w->static_opts.sendfile && conn->response_pending) {
        return 0;
    }

    while (conn->file_left > 0) {
        size_t chunk = conn->file_left;
        ssize_t moved;
        bool from_scratch = false;

        if (chunk > w->static_opts.sendfile_chunk) {
            chunk = w->static_opts.sendfile_chunk;
        }
        if (chunk > sizeof(scratch)) {
            chunk = sizeof(scratch);
        }
        if (w->static_opts.sendfile) {
            off_t offset = (off_t)conn->file_offset;

            moved = sendfile(conn->fd, conn->file_fd, &offset, chunk);
            if (moved < 0 && (errno == EINVAL || errno == ENOSYS)) {
                /*
                 * This filesystem cannot sendfile from the descriptor: read
                 * into the scratch buffer and send instead, for the rest of
                 * the transfer.
                 */
                from_scratch = true;
            }
        } else {
            from_scratch = true;
        }

        if (from_scratch) {
            /*
             * dx_send_all() reports success as 0 and failure as -1, so its
             * result must never be read as a byte count: that made a
             * successful chunk look like "file ended early" and truncated
             * every file at the first chunk boundary.
             *
             * It also cannot be used on a non-blocking socket: it reports
             * EAGAIN after a partial send and the unsent tail would be lost.
             * The bytes go through the connection buffer instead, which is
             * free once the headers are out, so a partial send is resumed by
             * the next EPOLLOUT with nothing lost and nothing resent.
             *
             * The file cursor must advance as soon as the bytes are copied
             * out of the file, before the socket is touched. Advancing it only
             * after a successful send meant that an EAGAIN left the parked
             * bytes in the buffer *and* the cursor where it was, so the next
             * attempt read the same region again and duplicated it.
             */
            moved = pread(conn->file_fd, scratch, chunk, (off_t)conn->file_offset);
            if (moved > 0) {
                size_t n = (size_t)moved;
                size_t space = dx_buffer_writable(&conn->buf);

                if (conn->buf.data == NULL || space == 0) {
                    /* No room to park the chunk; retry on the next event. */
                    conn->state = DX_CONN_WRITING;
                    conn->response_pending = true;
                    return 0;
                }
                if (n > space) {
                    n = space;
                }
                memcpy(conn->buf.data + conn->buf.len, scratch, n);
                conn->buf.len += n;
                conn->response_pending = true;
                conn->file_offset += (uint64_t)n;
                conn->file_left -= (uint64_t)n;
                /* conn_flush() counts these bytes when it actually sends them;
                 * counting them here as well reported ~2x output. */
                if (conn_flush(w, conn) != 0) {
                    return -1;
                }
                /* Parked or sent: either way this chunk is accounted for. */
                continue;
            }
        }

        if (moved > 0) {
            conn->file_offset += (uint64_t)moved;
            conn->file_left -= (uint64_t)moved;
            conn->bytes_out += (uint64_t)moved;
            continue;
        }
        if (moved < 0 && errno == EINTR) {
            continue;
        }
        if (moved < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The socket buffer is full: wait for EPOLLOUT and continue. */
            conn->state = DX_CONN_WRITING;
            return 0;
        }
        if (moved == 0) {
            /* The file is shorter than its stat size: stop instead of
             * spinning forever on an endless read. */
            dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: file ended early",
                         (unsigned long long)conn->id);
            conn->file_left = 0;
            break;
        }
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: sendfile: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }
    conn->state = DX_CONN_READING;
    return 0;
}

/*
 * Stream an in-memory response body (the directory listing) into the socket.
 * The headers are already out and the read buffer is free, so body chunks are
 * copied into it and handed to the same flush path as everything else. On
 * EAGAIN the remainder stays parked and the next EPOLLOUT resumes from
 * mem_body_off; nothing is re-sent and nothing is lost.
 */
static int conn_flush_mem_body(struct dx_worker *w, dx_conn_t *conn)
{
    while (conn->mem_body_off < conn->mem_body_len) {
        size_t want = conn->mem_body_len - conn->mem_body_off;
        size_t space;

        if (conn->buf.data == NULL) {
            return -1; /* unreachable: a live connection owns a buffer */
        }
        space = dx_buffer_writable(&conn->buf);
        if (space == 0) {
            conn->state = DX_CONN_WRITING;
            conn->response_pending = true;
            return 0;
        }
        if (want > space) {
            want = space;
        }
        memcpy(conn->buf.data + conn->buf.len, conn->mem_body + conn->mem_body_off, want);
        conn->buf.len += want;
        conn->mem_body_off += want;
        conn->response_pending = true;
        /* conn_flush() counts these when it sends them (see above). */

        if (conn_flush(w, conn) != 0) {
            return -1;
        }
        if (conn->response_pending) {
            return 0; /* EAGAIN: the rest is parked in the buffer */
        }
    }
    return 0;
}

/*
 * Arm the one timer this connection owns, choosing the deadline from its
 * current state. A refresh that moves less than a second is free, so calling
 * this on every event is cheap.
 */
void dx_conn_arm_timer(struct dx_worker *w, dx_conn_t *conn)
{
    uint64_t now;
    uint64_t delay = 0;

    if (w == NULL || conn == NULL || w->timers == NULL) {
        return;
    }
    now = (uint64_t)dx_now_ms();
    switch (conn->state) {
    case DX_CONN_WRITING:
        delay = w->life.send_timeout_ms;
        break;
    case DX_CONN_IDLE:
        delay = w->life.keepalive_timeout_ms;
        break;
    case DX_CONN_READING:
        delay = (conn->body_left > 0) ? w->life.body_timeout_ms : w->life.header_timeout_ms;
        break;
    case DX_CONN_FREE:    /* the slot is not owned by anybody */
    case DX_CONN_CLOSING: /* being torn down; its timer is already cancelled */
        return;
    }
    if (delay == 0) {
        dx_timer_wheel_cancel(w->timers, &conn->timer);
        return;
    }
    (void)dx_timer_wheel_refresh(w->timers, &conn->timer, delay, now);
    conn->timer.owner = conn;
}

int dx_conn_ensure_buffer(struct dx_worker *w, dx_conn_t *conn)
{
    unsigned char *mem;
    unsigned char *tbl;

    if (w == NULL || conn == NULL) {
        return -1;
    }
    if (conn->buf.data == NULL) {
        mem = (unsigned char *)dx_bufpool_acquire(w->bufs, DX_CONN_BUF_SIZE);
        if (mem == NULL) {
            dx_log_write(dx_worker_log(w), DX_LOG_WARN,
                         "closing connection: no read buffer available (%zu in use)",
                         dx_bufpool_outstanding(w->bufs));
            return -1;
        }
        conn->buf_mem = mem;
        dx_buffer_init(&conn->buf, mem, DX_CONN_BUF_SIZE);
    }
    /*
     * The header table is acquired together with the buffer: both are parsing
     * resources an idle connection does not need. The table is sized to the
     * compile-time maximum and the parser is capped to the configured
     * max_headers, so a reload can raise the limit without the pool shrinking.
     */
    if (conn->hdr_mem == NULL) {
        tbl = dx_bufpool_acquire(w->hdrs, DX_MAX_HEADERS_LIMIT * sizeof(dx_http_header_t));
        if (tbl == NULL) {
            dx_log_write(dx_worker_log(w), DX_LOG_WARN,
                         "closing connection: no header table available (%zu in use)",
                         dx_bufpool_outstanding(w->hdrs));
            return -1;
        }
        conn->hdr_mem = tbl;
        conn->hdr_table = (dx_http_header_t *)tbl;
        /*
         * The parser must be pointed at the fresh table before it sees another
         * byte. It was left without one when the connection went idle, so this
         * is a full reset, not a partial update.
         */
        dx_http_parser_init_ex(&conn->parser, &w->limits, conn->hdr_table, DX_MAX_HEADERS_LIMIT);
    }
    return 0;
}

/* Hand the header table back to the pool and leave the parser without one. */
void dx_conn_release_headers(dx_conn_t *conn, dx_bufpool_t *pool)
{
    if (conn == NULL || conn->hdr_mem == NULL) {
        return;
    }
    (void)dx_bufpool_release(pool, conn->hdr_mem);
    conn->hdr_mem = NULL;
    conn->hdr_table = NULL;
    /* The parser must never be read again before it is re-initialised. */
    dx_http_parser_init_ex(&conn->parser, NULL, NULL, 0u);
}

int dx_conn_on_writable(struct dx_worker *w, dx_conn_t *conn)
{
    if (conn == NULL || conn->state == DX_CONN_FREE || conn->state == DX_CONN_CLOSING) {
        return 0;
    }
    if (conn_flush(w, conn) != 0) {
        return -1;
    }
    /* Header buffer drained: continue the body that was streaming. */
    if (conn->file_left > 0 && conn_flush_file(w, conn) != 0) {
        return -1;
    }
    if (conn->mem_body_off < conn->mem_body_len && conn_flush_mem_body(w, conn) != 0) {
        return -1;
    }
    return conn_after_response(w, conn);
}

/* Bounded copy for access-log fields; always NUL terminates. */
static void acc_copy(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) {
        return;
    }
    if (len > cap - 1u) {
        len = cap - 1u; /* long values are truncated, never overflow */
    }
    if (len > 0 && src != NULL) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

/* True when the request method equals name exactly (methods are case sensitive). */
static bool method_is(const dx_http_request_t *req, const unsigned char *base, const char *name)
{
    size_t n = strlen(name);

    return req->method_len == n && memcmp(base + req->method_off, name, n) == 0;
}

/*
 * Record what the access logger needs. Called once per request with the parse
 * result still valid, or with req == NULL for requests that failed before a
 * method could be read.
 */
static void access_fill(dx_conn_t *conn, const dx_http_request_t *req, const unsigned char *base,
                        int status, size_t body_bytes)
{
    dx_access_t *acc = &conn->acc;

    acc->status = status;
    acc->body_bytes = body_bytes;
    if (req == NULL) {
        return;
    }
    acc->http10 = (req->minor_version == 0);
    acc_copy(acc->method, sizeof(acc->method), (const char *)base + req->method_off,
             req->method_len);
    acc_copy(acc->target, sizeof(acc->target), (const char *)base + req->target_off,
             req->target_len);
    {
        size_t voff;
        size_t vlen;

        if (dx_http_request_find_header(req, base, "Referer", &voff, &vlen)) {
            acc_copy(acc->referer, sizeof(acc->referer), (const char *)base + voff, vlen);
        }
        if (dx_http_request_find_header(req, base, "User-Agent", &voff, &vlen)) {
            acc_copy(acc->user_agent, sizeof(acc->user_agent), (const char *)base + voff, vlen);
        }
    }
}

/*
 * Stage the response bytes for the flush loop. The caller consumed the request
 * bytes first and the buffer belongs to this connection alone, so overwriting
 * it is safe; the pipelined bytes that followed are lost unless the caller
 * copied them out (see conn_process_requests()).
 */
static int conn_stage_response(dx_conn_t *conn, const char *resp, size_t len)
{
    if (conn->buf.data == NULL || len > conn->buf.cap) {
        return -1;
    }
    memcpy(conn->buf.data, resp, len);
    conn->buf.off = 0;
    conn->buf.len = len;
    conn->response_pending = true;
    return 0;
}

/*
 * Swallow input that is already in flight after a protocol error, so the
 * close() that follows does not turn into an RST that destroys the queued
 * error response before the client can read it. Bounded: an abusive client
 * that keeps sending past the cap gets its RST after all.
 */
#define DX_DISCARD_CAP (64u * 1024u)
#define DX_DISCARD_CHUNK 4096u

static void conn_discard_input(dx_conn_t *conn)
{
    unsigned char scratch[DX_DISCARD_CHUNK];
    size_t total = 0;

    while (total < DX_DISCARD_CAP) {
        ssize_t n = read(conn->fd, scratch, sizeof(scratch));

        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            conn->eof = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break; /* EAGAIN: the socket is drained for now */
    }
    conn->bytes_in += total;
}

/*
 * Serve a static request end to end: resolve the target, open the file,
 * evaluate the conditional and Range headers, stage the response headers and
 * arm the file transfer.
 *
 * Returns 0 when the response was staged (a body may still be streaming) and
 * -1 on a fatal error. Whether the connection survives the response is
 * decided by response_keeps_alive(); this function only sets the flag.
 */
/*
 * Stage the response bytes, push them out (headers first, then the streamed
 * file body) and run the post-response step. Single exit point for every
 * response so the keep-alive rules cannot be forgotten on one path.
 */
static int conn_finish(struct dx_worker *w, dx_conn_t *conn, size_t consumed, const char *resp,
                       size_t len)
{
    size_t leftover;

    if (len == 0 || conn->buf.data == NULL || consumed > dx_buffer_readable(&conn->buf)) {
        return -1;
    }
    /*
     * Drop the request bytes, then park whatever the client sent behind them.
     * The response is staged over the read buffer, so anything still in it
     * would be destroyed. A kept-alive connection must answer the pipelined
     * request that way; a closing one drops it, which is what the protocol
     * allows for an error response and saves the copy.
     */
    dx_buffer_consume(&conn->buf, consumed);
    leftover = dx_buffer_readable(&conn->buf);
    if (leftover > 0 && !conn->close_conn && conn_hold_pipelined(conn, leftover) != 0) {
        return -1;
    }
    if (conn_stage_response(conn, resp, len) != 0) {
        return -1;
    }
    if (conn_flush(w, conn) != 0) {
        return -1;
    }
    if (conn->file_left > 0 && conn_flush_file(w, conn) != 0) {
        return -1;
    }
    if (conn->mem_body_off < conn->mem_body_len && conn_flush_mem_body(w, conn) != 0) {
        return -1;
    }
    return conn_after_response(w, conn);
}

/* Stage an error page: these responses always end the connection. */
static int conn_finish_error(struct dx_worker *w, dx_conn_t *conn, int status,
                             const char *extra_headers)
{
    char tmp[DX_RESPONSE_TMP];
    size_t len = dx_http_response_error_ex(tmp, sizeof(tmp), status, extra_headers, "close",
                                           w->static_opts.server_tokens);

    conn->close_conn = true;
    /* The connection ends here, so the buffered request bytes are dropped
     * rather than copied out: nothing will ever be answered from them. */
    return conn_finish(w, conn, dx_buffer_readable(&conn->buf), tmp, len);
}

/*
 * Serve a static request end to end: resolve the target, open the file,
 * evaluate the conditional and Range headers, stage the response headers and
 * arm the file transfer.
 */
static int conn_serve_static(struct dx_worker *w, dx_conn_t *conn, const dx_http_request_t *req,
                             const unsigned char *base, bool head, size_t consumed)
{
    const char *target = (const char *)base + req->target_off;
    size_t target_len = req->target_len;
    char tmp[DX_RESPONSE_TMP];
    char extra[768];
    char location[DX_LOCATION_MAX];
    char served_file[DX_PATH_MAX];
    size_t extra_len = 0;
    size_t location_len = 0;
    size_t len;
    int status;
    dx_path_t resolved;
    dx_path_opts_t path_opts;
    dx_open_result_t opened;
    dx_open_t open_result;
    bool keep_alive;
    const char *connection;
    size_t voff;
    size_t vlen;

    /* 1. Target to filesystem path. */
    path_opts.hidden_files = w->static_opts.hidden_files;
    status = dx_path_resolve(target, target_len, &path_opts, &resolved);
    if (status != 0) {
        access_fill(conn, req, base, status, 0);
        return conn_finish_error(w, conn, status, NULL);
    }
    if (w->fs == NULL) {
        access_fill(conn, req, base, 404, 0);
        return conn_finish_error(w, conn, 404, NULL);
    }

    /* 2. Open below the document root. */
    open_result =
        dx_fs_open(w->fs, resolved.path, resolved.had_trailing_slash, location, sizeof(location),
                   &location_len, served_file, sizeof(served_file), &opened);

    keep_alive =
        response_keeps_alive(w, conn, req, base, (open_result == DX_OPEN_REDIRECT) ? 301 : 200);
    connection = connection_value(keep_alive, req->minor_version);
    conn->close_conn = !keep_alive;

    if (open_result == DX_OPEN_REDIRECT) {
        extra_len = (size_t)snprintf(extra, sizeof(extra), "Location: %.*s\r\n", (int)location_len,
                                     location);
        if (extra_len >= sizeof(extra)) {
            return conn_finish_error(w, conn, 500, NULL);
        }
        access_fill(conn, req, base, 301, 0);
        len = dx_http_response_build_ex(tmp, sizeof(tmp), 301, extra, NULL, NULL, 0u, true,
                                        connection, w->static_opts.server_tokens);
        return conn_finish(w, conn, consumed, tmp, len);
    }

    if (open_result == DX_OPEN_LISTING) {
        size_t body_len;

        /*
         * A listing does not fit in the staging buffer: with a few hundred
         * entries the response could not be built, so the connection was
         * closed and the client received nothing at all. The body is rendered
         * into an owned buffer and streamed through the socket exactly like a
         * file body, so its size no longer caps the listing.
         *
         * Only the previous in-memory body is released here. `base` points
         * into conn->hdr when the request body was drained incrementally, and
         * access_fill() below still reads through it, so freeing the header
         * block before that call was a heap-use-after-free (caught by ASan on
         * a GET with a Content-Length whose body arrived in a later segment).
         * conn->hdr is released by dx_conn_reset() when the slot is recycled.
         */
        free(conn->mem_body);
        conn->mem_body = NULL;
        conn->mem_body_len = 0;
        conn->mem_body_off = 0;
        conn->mem_body = (unsigned char *)malloc(DX_LISTING_TMP);
        if (conn->mem_body == NULL) {
            access_fill(conn, req, base, 500, 0);
            return conn_finish_error(w, conn, 500, NULL);
        }
        body_len = dx_fs_list(w->fs, resolved.path, (char *)conn->mem_body, DX_LISTING_TMP);
        if (body_len == 0) {
            access_fill(conn, req, base, 403, 0);
            return conn_finish_error(w, conn, 403, NULL);
        }
        conn->mem_body_len = body_len;
        conn->mem_body_off = 0;
        /*
         * dx_http_response_headers() writes Content-Length itself. Adding one
         * here as well produced two framing headers on every listing, which is
         * both invalid (RFC 9110 8.6) and a request-smuggling primitive in
         * front of a proxy. The length is the full body for HEAD too, so the
         * HEAD response matches its GET twin.
         */
        access_fill(conn, req, base, 200, head ? 0u : body_len);
        len = dx_http_response_headers(tmp, sizeof(tmp), 200, NULL, "text/html; charset=utf-8",
                                       body_len, head, connection, w->static_opts.server_tokens);
        if (len == 0) {
            dx_conn_release_scratch(conn);
            return conn_finish_error(w, conn, 500, NULL);
        }
        if (head) {
            /* HEAD sends the headers only, so the rendered body is dead. */
            free(conn->mem_body);
            conn->mem_body = NULL;
            conn->mem_body_len = 0;
            conn->mem_body_off = 0;
        }
        return conn_finish(w, conn, consumed, tmp, len);
    }

    if (open_result != DX_OPEN_OK) {
        status =
            (open_result == DX_OPEN_FORBIDDEN) ? 403 : ((open_result == DX_OPEN_ERROR) ? 500 : 404);
        access_fill(conn, req, base, status, 0);
        return conn_finish_error(w, conn, status, NULL);
    }

    /* 3. Validators for the opened file. */
    {
        long long mtime_sec = (long long)opened.st.st_mtime;
        uint64_t size = (uint64_t)opened.st.st_size;
        char last_modified[40];
        char etag[64];
        bool not_modified = false;

        etag[0] = '\0';
        if (w->static_opts.etag && mtime_sec >= 0) {
            (void)dx_http_etag_format(etag, sizeof(etag), mtime_sec, size);
        }
        if (dx_http_date(last_modified, sizeof(last_modified), mtime_sec * 1000LL) == 0) {
            last_modified[0] = '\0';
        }

        extra_len = 0;
        if (last_modified[0] != '\0') {
            extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                          "Last-Modified: %s\r\n", last_modified);
        }
        if (etag[0] != '\0') {
            if (extra_len < sizeof(extra)) {
                extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                              "ETag: %s\r\n", etag);
            }
        }
        if (extra_len < sizeof(extra) && w->static_opts.cache_control != NULL &&
            w->static_opts.cache_control[0] != '\0') {
            extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                          "Cache-Control: %s\r\n", w->static_opts.cache_control);
        }
        /*
         * snprintf returns the length it *would* have written, so extra_len can
         * exceed sizeof(extra) after a truncation. Every append above is
         * therefore guarded by `extra_len < sizeof(extra)`, and the final check
         * rejects the request instead of letting the next append compute a
         * wrapped (huge) size.
         */
        if (extra_len >= sizeof(extra)) {
            (void)close(opened.fd);
            return conn_finish_error(w, conn, 500, NULL);
        }

        /* Conditional requests: If-None-Match wins over If-Modified-Since. */
        if (etag[0] != '\0' &&
            dx_http_request_find_header(req, base, "If-None-Match", &voff, &vlen) &&
            dx_http_etag_matches((const char *)base + voff, vlen, etag)) {
            not_modified = true;
        } else if (dx_http_request_find_header(req, base, "If-Modified-Since", &voff, &vlen) &&
                   dx_http_modified_since((const char *)base + voff, vlen, mtime_sec)) {
            not_modified = true;
        }
        if (not_modified) {
            access_fill(conn, req, base, 304, 0);
            len = dx_http_response_build_ex(tmp, sizeof(tmp), 304, extra, NULL, NULL, 0u, true,
                                            connection, w->static_opts.server_tokens);
            (void)close(opened.fd);
            return conn_finish(w, conn, consumed, tmp, len);
        }

        /* 4. Range handling (GET only; HEAD always describes the full file). */
        {
            char content_type[160];
            int written;
            dx_range_t range;
            dx_range_result_t range_result = DX_RANGE_NONE;
            uint64_t start = 0;
            uint64_t body_len = size;
            bool partial = false;

            if (!head && dx_http_request_find_header(req, base, "Range", &voff, &vlen)) {
                range_result = dx_http_range_parse((const char *)base + voff, vlen, size, &range);
                /*
                 * If-Range only switches to a full response when a validator
                 * was given and does not match.
                 */
                if (range_result == DX_RANGE_OK &&
                    dx_http_request_find_header(req, base, "If-Range", &voff, &vlen) &&
                    !dx_http_if_range_match((const char *)base + voff, vlen, etag, mtime_sec)) {
                    range_result = DX_RANGE_NONE;
                }
            }

            if (range_result == DX_RANGE_UNSATISFIABLE) {
                /*
                 * 416 is an error status: like every other error path it must
                 * end the connection. keep_alive/connection were computed for a
                 * 200 before the Range header was read, so they have to be
                 * overridden here or the client is told to keep the connection
                 * alive on an error (contradicting response_keeps_alive()).
                 */
                connection = "close";
                conn->close_conn = true;
                written = snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                   "Content-Range: bytes */%llu\r\nAccept-Ranges: bytes\r\n",
                                   (unsigned long long)size);

                access_fill(conn, req, base, 416, 0);
                (void)close(opened.fd);
                if (written < 0 || (size_t)written >= sizeof(extra) - extra_len) {
                    return conn_finish_error(w, conn, 500, NULL);
                }
                len = dx_http_response_build_ex(tmp, sizeof(tmp), 416, extra, NULL, NULL, 0u, true,
                                                connection, w->static_opts.server_tokens);
                return conn_finish(w, conn, consumed, tmp, len);
            }

            if (range_result == DX_RANGE_OK) {
                start = range.start;
                body_len = range.length;
                partial = true;
                /*
                 * Content-Range is mandatory on a 206 (RFC 9110 15.3.7). If it
                 * does not fit the buffer the request must fail with 500:
                 * silently skipping the header would emit a 206 with no way
                 * for the client to tell which bytes it received.
                 */
                written = snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                   "Content-Range: bytes %llu-%llu/%llu\r\n",
                                   (unsigned long long)range.start, (unsigned long long)range.end,
                                   (unsigned long long)size);
                if (written < 0 || (size_t)written >= sizeof(extra) - extra_len) {
                    (void)close(opened.fd);
                    return conn_finish_error(w, conn, 500, NULL);
                }
                extra_len += (size_t)written;
            }
            if (extra_len < sizeof(extra)) {
                extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                              "Accept-Ranges: bytes\r\n");
            }
            /* Same truncation guard as above: extra_len can exceed the buffer. */
            if (extra_len >= sizeof(extra)) {
                (void)close(opened.fd);
                return conn_finish_error(w, conn, 500, NULL);
            }

            (void)dx_mime_content_type(w->mime,
                                       (served_file[0] != '\0') ? served_file : resolved.path,
                                       content_type, sizeof(content_type));

            status = partial ? 206 : 200;
            access_fill(conn, req, base, status, head ? 0u : (size_t)body_len);
            len = dx_http_response_headers(tmp, sizeof(tmp), status, extra, content_type,
                                           (size_t)body_len, head, connection,
                                           w->static_opts.server_tokens);
            if (len == 0) {
                (void)close(opened.fd);
                return conn_finish_error(w, conn, 500, NULL);
            }

            if (head) {
                /* HEAD sends identical headers and no body. */
                (void)close(opened.fd);
                return conn_finish(w, conn, consumed, tmp, len);
            }
            /* Arm the transfer; the body is streamed after the headers. */
            conn->file_fd = opened.fd;
            conn->file_offset = start;
            conn->file_left = body_len;
        }
    }
    /*
     * conn_finish() drops the request bytes and parks the pipelined bytes
     * before the response overwrites the read buffer.
     */
    return conn_finish(w, conn, consumed, tmp, len);
}

/*
 * Header token search: does a comma separated header value contain token?
 * Case insensitive, with optional whitespace around the commas.
 */
static bool header_has_token(const char *value, size_t len, const char *token)
{
    size_t token_len = strlen(token);
    size_t pos = 0;

    while (pos < len) {
        size_t start;
        size_t item_len;

        while (pos < len && (value[pos] == ',' || value[pos] == ' ' || value[pos] == '\t')) {
            pos++;
        }
        start = pos;
        while (pos < len && value[pos] != ',') {
            pos++;
        }
        item_len = pos - start;
        while (item_len > 0 &&
               (value[start + item_len - 1u] == ' ' || value[start + item_len - 1u] == '\t')) {
            item_len--;
        }
        if (item_len == token_len && strncasecmp(value + start, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Decide whether the connection survives the response that was just built.
 * The rules come straight from the README: error statuses, a client requested
 * close, an explicit HTTP/1.0 without keep-alive, the request budget and
 * shutdown all end the connection.
 */
static bool response_keeps_alive(const struct dx_worker *w, const dx_conn_t *conn,
                                 const dx_http_request_t *req, const unsigned char *base,
                                 int status)
{
    size_t voff;
    size_t vlen;

    if (status >= 400 || req == NULL || conn->eof || w->draining) {
        return false;
    }
    if (w->life.keepalive_requests > 0 && conn->requests + 1u >= w->life.keepalive_requests) {
        return false;
    }
    if (dx_http_request_find_header(req, base, "Connection", &voff, &vlen)) {
        const char *value = (const char *)base + voff;

        if (header_has_token(value, vlen, "close")) {
            return false;
        }
        if (req->minor_version == 0) {
            /* HTTP/1.0 needs an explicit keep-alive. */
            return header_has_token(value, vlen, "keep-alive");
        }
    }
    return req->minor_version == 1;
}

/* Connection header value that belongs in the response being built. */
static const char *connection_value(bool keep_alive, int minor_version)
{
    if (!keep_alive) {
        return "close";
    }
    return (minor_version == 0) ? "keep-alive" : NULL; /* 1.1 implies keep-alive */
}

/*
 * Move the bytes that follow a request out of the read buffer before the
 * response overwrites it. Only pipelining needs this, so the holdover is
 * allocated on that path and freed as soon as the next request consumed it.
 */
static int conn_hold_pipelined(dx_conn_t *conn, size_t leftover)
{
    unsigned char *saved;

    if (leftover == 0) {
        return 0;
    }
    saved = (unsigned char *)malloc(leftover);
    if (saved == NULL) {
        return -1;
    }
    memcpy(saved, conn->buf.data + conn->buf.off, leftover);
    conn->hold = saved;
    conn->hold_len = leftover;
    return 0;
}

/* Put held pipelined bytes back in front of the read buffer. */
static int conn_restore_pipelined(struct dx_worker *w, dx_conn_t *conn)
{
    size_t len = conn->hold_len;

    if (len == 0) {
        return 0;
    }
    if (dx_conn_ensure_buffer(w, conn) != 0) {
        free(conn->hold);
        conn->hold = NULL;
        conn->hold_len = 0;
        return -1;
    }
    if (len > conn->buf.cap && conn_grow_to(w, conn, len) != 0) {
        /*
         * The holdover no longer fits: it can happen when a client pipelines
         * a request behind a body that was drained out of the buffer. Growing
         * is bounded by the header limit, so an oversized pipelined request is
         * refused rather than trusted.
         */
        dx_conn_release_hold(conn);
        return -1;
    }
    memcpy(conn->buf.data, conn->hold, len);
    conn->buf.off = 0;
    conn->buf.len = len;
    free(conn->hold);
    conn->hold = NULL;
    conn->hold_len = 0;
    return 0;
}

static int conn_process(struct dx_worker *w, dx_conn_t *conn);

/*
 * Called when the last byte of a response is on the wire. Closes the
 * connection when the response said so, otherwise resets for the next request:
 * pipelined bytes are answered first, and an empty connection goes idle with
 * its read buffer released.
 */
static int conn_after_response(struct dx_worker *w, dx_conn_t *conn)
{
    if (dx_conn_has_work(conn) || dx_conn_has_pending(conn)) {
        return 0; /* more to send */
    }
    /* The body is fully out: the scratch copy is dead weight from here on. */
    if (conn->mem_body != NULL) {
        dx_conn_release_scratch(conn);
    }
    conn->requests++;
    /*
     * Emit the access line now, while this request's record is intact. The
     * old code logged only in conn_close(), so a keep-alive connection with
     * four requests produced a single line describing the last one.
     */
    dx_worker_emit_access(w, conn);
    if (conn->close_conn || conn->eof) {
        return -1;
    }

    /* Ready for the next request on the same connection. */
    dx_http_parser_init_ex(&conn->parser, &w->limits, conn->hdr_table,
                           (conn->hdr_table != NULL) ? DX_MAX_HEADERS_LIMIT : 0u);
    conn->close_conn = false;
    conn->body_left = 0;
    conn->state = DX_CONN_READING;

    if (conn_restore_pipelined(w, conn) != 0) {
        return -1;
    }
    /*
     * A pipelined request is now sitting in the buffer. It is parsed by
     * conn_process's own loop (the caller of conn_finish), so returning here
     * with the buffer held does not stall the connection -- and it removes the
     * recursion that used to overflow the stack on a long pipelined burst.
     *
     * With nothing pipelined the connection is idle: the read buffer goes back
     * to the pool and the KEEPALIVE timeout is armed. Skipping that (as an
     * earlier rewrite did, by returning unconditionally) left every idle
     * keep-alive connection holding its buffer and closed by the *header*
     * timeout instead.
     */
    if (dx_conn_has_pending(conn)) {
        return 0; /* a restored pipelined request: the caller's loop parses it */
    }
    /* Idle: the read buffer and header table go back to the pools. */
    dx_conn_release_buffer(conn, w->bufs);
    dx_conn_release_headers(conn, w->hdrs);
    conn->state = DX_CONN_IDLE;
    dx_conn_arm_timer(w, conn);
    return 0;
}

/*
 * Feed the parser and, when a request is complete (or rejected), build and
 * queue the response. Returns 0 while the connection stays open, -1 when it
 * must close now.
 */
/*
 * JSON status endpoint. Only clients from loopback may read it (127.0.0.0/8
 * and ::1): the counters are operationally sensitive and the README documents
 * the loopback-only rule. A non-loopback client gets 404 so the endpoint is
 * not even disclosed.
 */
static bool peer_is_loopback(const char *peer)
{
    if (peer == NULL) {
        return false;
    }
    if (strcmp(peer, "::1") == 0) {
        return true;
    }
    /*
     * ::ffff:127.0.0.1 is the same loopback address seen through a dual-stack
     * listener (INET6_ADDRSTRLEN formatting of a v4-mapped v6 peer).
     */
    if (strncmp(peer, "::ffff:127.", 11) == 0) {
        return true;
    }
    /* 127.0.0.0/8 */
    return strncmp(peer, "127.", 4) == 0;
}

static bool target_equals(const dx_http_request_t *req, const unsigned char *base, const char *path)
{
    size_t n = strlen(path);

    return req->target_len == n && memcmp(base + req->target_off, path, n) == 0;
}

/* Build and stage the JSON status response. Returns 0 or -1 (close). */
static int conn_serve_status(struct dx_worker *w, dx_conn_t *conn, const dx_http_request_t *req,
                             const unsigned char *base, bool head, size_t consumed)
{
    char body[DX_RESPONSE_TMP];
    char tmp[DX_RESPONSE_TMP];
    uint64_t uptime;
    size_t body_len;
    size_t len;
    bool keep_alive;
    const char *connection;

    uptime = (uint64_t)((dx_now_ms() - (long long)w->start_ms) / 1000);
    body_len =
        dx_status_json(w->status, body, sizeof(body), DX_VERSION_STRING, uptime, w->total_workers);
    if (body_len == 0) {
        return conn_finish_error(w, conn, 500, NULL);
    }

    keep_alive = response_keeps_alive(w, conn, req, base, 200);
    connection = connection_value(keep_alive, req->minor_version);
    conn->close_conn = !keep_alive;

    len = dx_http_response_build_ex(tmp, sizeof(tmp), 200, NULL, "application/json", body, body_len,
                                    head, connection, w->static_opts.server_tokens);
    if (len == 0) {
        return conn_finish_error(w, conn, 500, NULL);
    }
    access_fill(conn, req, base, 200, head ? 0u : body_len);
    return conn_finish(w, conn, consumed, tmp, len);
}

/*
 * Build and stage the answer for a request whose headers were parsed and
 * whose body (if any) has been fully received. `consumed` covers the header
 * block plus the body, so the bytes are only dropped here.
 */
static int conn_dispatch(struct dx_worker *w, dx_conn_t *conn)
{
    char tmp[DX_RESPONSE_TMP];
    size_t consumed;
    size_t len = 0;
    int status = 0;
    const char *extra = NULL;
    size_t body_bytes = 0;
    const unsigned char *base;
    const dx_http_request_t *req;

    if (conn->buf.data == NULL) {
        return -1; /* unreachable: a live connection owns a buffer */
    }
    /*
     * While a body was drained the header block lives in conn->hdr; the read
     * buffer then only holds body bytes, so the request length is whatever is
     * left in it.
     */
    base = (conn->hdr != NULL) ? conn->hdr : conn->buf.data + conn->buf.off;
    req = &conn->parser.req;
    consumed = conn_request_bytes(conn);

    /* Decide what to answer. */
    {
        bool head = method_is(req, base, "HEAD");
        bool is_options = method_is(req, base, "OPTIONS");
        bool keep_alive;
        const char *connection;
        dx_dispatch_t action = dx_http_dispatch_method(req, base, &status, &extra);

        /*
         * The JSON status endpoint is answered before the static handler so it
         * works even without a document root, and only for loopback clients.
         * It is read-only: any method other than GET or HEAD is rejected with
         * 405, so a POST to the metrics path cannot pretend to have changed
         * anything and still learn the snapshot.
         */
        if (w->status_path != NULL && w->status_path[0] != '\0' &&
            target_equals(req, base, w->status_path)) {
            if (!peer_is_loopback(conn->peer)) {
                /* Not disclosed to non-loopback clients. */
                status = 404;
                extra = NULL;
                access_fill(conn, req, base, 404, 0);
                conn->close_conn = true;
                len = dx_http_response_error_ex(tmp, sizeof(tmp), 404, NULL, "close",
                                                w->static_opts.server_tokens);
                if (len == 0) {
                    dx_log_write(dx_worker_log(w), DX_LOG_ERROR,
                                 "conn %llu: response did not fit the staging buffer",
                                 (unsigned long long)conn->id);
                    return -1;
                }
                return conn_finish(w, conn, consumed, tmp, len);
            }
            if (head || method_is(req, base, "GET")) {
                return conn_serve_status(w, conn, req, base, head, consumed);
            }
            /* 405 for every other method, with the supported list. */
            status = 405;
            extra = "Allow: GET, HEAD\r\n";
            access_fill(conn, req, base, 405, 0);
            conn->close_conn = true;
            len = dx_http_response_error_ex(tmp, sizeof(tmp), 405, extra, "close",
                                            w->static_opts.server_tokens);
            if (len == 0) {
                dx_log_write(dx_worker_log(w), DX_LOG_ERROR,
                             "conn %llu: response did not fit the staging buffer",
                             (unsigned long long)conn->id);
                return -1;
            }
            return conn_finish(w, conn, consumed, tmp, len);
        }

        if (action == DX_DISPATCH_SERVE) {
            status = 200;
            if (!is_options && w->fs != NULL) {
                /* Static file serving: the response may arm a file transfer,
                 * which continues in conn_flush_file() after the headers.
                 * The request bytes are not consumed yet: the resolver and
                 * the header lookups still read them through base. */
                return conn_serve_static(w, conn, req, base, head, consumed);
            }
        }
        keep_alive = response_keeps_alive(w, conn, req, base, status);
        connection = connection_value(keep_alive, req->minor_version);
        conn->close_conn = !keep_alive;

        if (action == DX_DISPATCH_SERVE) {
            if (is_options) {
                extra = "Allow: GET, HEAD, OPTIONS\r\n";
                len = dx_http_response_build_ex(tmp, sizeof(tmp), status, extra, NULL, NULL, 0u,
                                                false, connection, w->static_opts.server_tokens);
            } else {
                /* No document root configured: the core still answers. */
                len = dx_http_response_build_ex(
                    tmp, sizeof(tmp), status, NULL, "text/plain; charset=utf-8", conn_hello_body,
                    sizeof(conn_hello_body) - 1u, head, connection, w->static_opts.server_tokens);
                body_bytes = head ? 0u : sizeof(conn_hello_body) - 1u;
            }
        } else {
            len = dx_http_response_error_ex(tmp, sizeof(tmp), status, extra, connection,
                                            w->static_opts.server_tokens);
        }
    }

    if (len == 0) {
        dx_log_write(dx_worker_log(w), DX_LOG_ERROR,
                     "conn %llu: response did not fit the staging buffer",
                     (unsigned long long)conn->id);
        return -1;
    }

    /* Log fields must be captured while the request bytes are still valid. */
    access_fill(conn, req, base, status, body_bytes);

    return conn_finish(w, conn, consumed, tmp, len);
}

/*
 * Hand the read buffer over to a request body that is still arriving.
 *
 * The header block is copied into an owned buffer and the body bytes that
 * already arrived are moved to the front, so the buffer only ever has to hold
 * one window of the body instead of the whole thing. The parser addresses the
 * request by offsets into the header block, so the copy keeps them valid.
 *
 * Returns 0 on success, -1 when the copy could not be allocated.
 */
static int conn_park_headers(dx_conn_t *conn, size_t header_len)
{
    unsigned char *saved;
    size_t buffered = dx_buffer_readable(&conn->buf);
    size_t body_in_buf = (buffered > header_len) ? buffered - header_len : 0u;

    if (header_len == 0) {
        return -1;
    }
    saved = (unsigned char *)malloc(header_len);
    if (saved == NULL) {
        return -1;
    }
    memcpy(saved, conn->buf.data + conn->buf.off, header_len);
    dx_conn_release_scratch(conn); /* a stale copy must never be mixed in */
    conn->hdr = saved;
    conn->hdr_len = header_len;

    if (body_in_buf > 0) {
        memmove(conn->buf.data, conn->buf.data + conn->buf.off + header_len, body_in_buf);
    }
    conn->buf.off = 0;
    conn->buf.len = body_in_buf;
    conn->body_offset = 0;
    return 0;
}

/*
 * Feed the parser and act on the result: answer a rejection, start or finish
 * the body drain, or dispatch a request that needs no body.
 * Returns 0 while the connection stays open, -1 when it must close now.
 */
static int conn_process(struct dx_worker *w, dx_conn_t *conn)
{
    size_t consumed = 0;

    if (conn->buf.data == NULL) {
        return -1; /* unreachable: a live connection owns a buffer */
    }

    /*
     * One iteration per pipelined request. The loop replaces the previous
     * tail call chain (conn_finish -> conn_after_response -> conn_process),
     * which recursed once per buffered request: with the large staging
     * buffers of conn_dispatch on every frame, a few hundred pipelined
     * requests exhausted the stack in the non-optimised and sanitizer builds
     * (measured: stack-overflow at ~10 frames with a 256 KiB stack).
     */
    for (;;) {
        dx_parse_result_t r;
        const unsigned char *base;
        const dx_http_request_t *req;

        r = dx_http_parser_feed(&conn->parser, conn->buf.data + conn->buf.off,
                                dx_buffer_readable(&conn->buf), &consumed);
        if (r == DX_PARSE_NEED_DATA) {
            return 0; /* wait for more bytes */
        }

        if (r == DX_PARSE_ERROR) {
            /* The request is rejected before any method is known. */
            int status = conn->parser.status_code;

            access_fill(conn, NULL, NULL, status, 0);
            conn->close_conn = true;
            /*
             * The client may still be sending the oversized or malformed
             * request; discard what is already in flight so the error response
             * is read instead of lost to an RST.
             */
            conn_discard_input(conn);
            return conn_finish_error(w, conn, status, NULL);
        }

        base = conn->buf.data + conn->buf.off;
        req = &conn->parser.req;

        /*
         * A request body is read and discarded before the response goes out, so
         * the next request on a kept-alive connection starts at a clean boundary.
         */
        if (req->has_content_length && req->content_length > 0) {
            if (req->content_length > w->life.max_body_size) {
                access_fill(conn, req, base, 413, 0);
                conn_discard_input(conn);
                return conn_finish_error(w, conn, 413, NULL);
            }
            conn->body_left = req->content_length;
            conn->body_offset = consumed;
            conn->state = DX_CONN_READING;
            /*
         * Dispatch still reads method, target and header values through the
         * offsets the parser produced, so the header block must survive until
         * the body is complete. It is moved into an owned copy (conn->hdr) and
         * the read buffer is recycled for body bytes, which is what keeps a
         * body larger than the buffer from dead-ending the connection.
         */
            {
                size_t available = dx_buffer_readable(&conn->buf) - consumed;

                if (available >= conn->body_left) {
                    /* The body was already here (pipelined or eager client). */
                    conn->body_left = 0;
                } else {
                    if (conn_park_headers(conn, consumed) != 0) {
                        return -1;
                    }
                    /* The body bytes now sit at offset 0 and are all uncounted. */
                    available = dx_buffer_readable(&conn->buf);
                    conn->body_left -= available;
                    dx_buffer_consume(&conn->buf, available);
                    conn->body_offset = 0;
                    dx_conn_arm_timer(w, conn);
                    return 0; /* the rest of the body arrives with later reads */
                }
            }
        }
        conn->body_offset = consumed;
        dx_conn_release_scratch(conn); /* no saved header block for this request */
        if (conn_dispatch(w, conn) != 0) {
            return -1;
        }
        /*
         * The response is out. When a pipelined request is already sitting in
         * the buffer, loop and parse it here instead of recursing through
         * conn_after_response (which conn_finish calls). Anything else --
         * closing, idle, a partially sent response -- is left to the caller.
         */
        if (conn->state != DX_CONN_READING || !dx_conn_has_pending(conn) ||
            dx_conn_has_work(conn)) {
            return 0;
        }
    }
}

/*
 * Double the read buffer for requests larger than the pooled slot size. Growth
 * is bounded by the configured header limit, so a client can never make the
 * server allocate without end. The live bytes move to a fresh allocation with
 * the request start at offset 0, which keeps every parser offset valid.
 * Returns 0 on success, -1 when the limit is reached or malloc fails.
 */
static int conn_grow_to(struct dx_worker *w, dx_conn_t *conn, size_t need)
{
    size_t cap_max = w->limits.max_header_size + 4u;
    size_t want;
    size_t live = dx_buffer_readable(&conn->buf);
    unsigned char *fresh;

    if (conn->buf.cap >= cap_max) {
        return -1; /* limit reached: the parser answers 431 before this point */
    }
    if (need > cap_max) {
        return -1; /* cannot ever hold need bytes */
    }
    want = conn->buf.cap;
    if (want == 0) {
        want = DX_CONN_BUF_SIZE;
    }
    while (want < need) {
        want *= 2u;
    }
    if (want <= conn->buf.cap) {
        want = cap_max; /* also guards the theoretical cap overflow */
    }
    if (want > cap_max) {
        want = cap_max; /* clamp: the parser must still see its limit byte */
    }
    fresh = (unsigned char *)malloc(want);
    if (fresh == NULL) {
        return -1;
    }
    if (live > 0) {
        memcpy(fresh, conn->buf.data + conn->buf.off, live);
    }
    if (conn->buf_mem != NULL) {
        (void)dx_bufpool_release(w->bufs, conn->buf_mem);
        conn->buf_mem = NULL;
    } else {
        free(conn->buf.data);
    }
    dx_buffer_init(&conn->buf, fresh, want);
    conn->buf.len = live;
    return 0;
}

/* Double the read buffer for a request larger than the pooled slot size. */
static int conn_grow(struct dx_worker *w, dx_conn_t *conn)
{
    return conn_grow_to(w, conn, conn->buf.cap + 1u);
}

int dx_conn_on_readable(struct dx_worker *w, dx_conn_t *conn)
{
    /*
     * A bounded read budget keeps one talkative connection from monopolising
     * the event loop: after this many syscalls the connection yields and the
     * remaining data is picked up by the next EPOLLIN notification.
     */
    size_t budget = 64u;

    if (conn == NULL || conn->state == DX_CONN_FREE || conn->state == DX_CONN_CLOSING) {
        return 0;
    }
    if (dx_conn_has_work(conn)) {
        /*
         * A response is still on its way out. Responses keep their order, so
         * nothing new is parsed; the interest mask already dropped EPOLLIN.
         */
        return 0;
    }
    if (dx_conn_ensure_buffer(w, conn) != 0) {
        return -1;
    }
    conn->state = DX_CONN_READING;

    while (budget > 0) {
        ssize_t n;

        budget--;

        if (dx_buffer_writable(&conn->buf) == 0) {
            if (conn_grow(w, conn) == 0) {
                continue; /* more capacity: keep reading */
            }
            /*
             * The parser answers 431 once it sees the byte past the header
             * limit; if the buffer cannot grow, that byte can never arrive.
             * Closing is the only safe move (a malformed or oversized client
             * must not spin the loop).
             */
            dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: request buffer exhausted",
                         (unsigned long long)conn->id);
            return -1;
        }

        n = read(conn->fd, conn->buf.data + conn->buf.len, dx_buffer_writable(&conn->buf));
        if (n > 0) {
            conn->buf.len += (size_t)n;
            conn->bytes_in += (size_t)n;

            if (conn->body_left > 0) {
                /*
                 * Request body: discard it so the connection stream stays
                 * aligned for the next keep-alive request.
                 *
                 * Every body byte in the buffer is counted exactly once and
                 * dropped right away, so "how much arrived" can never be
                 * confused with "how much is still buffered". The earlier code
                 * recomputed the buffered total on every read and subtracted it
                 * again, so body_left fell faster than bytes arrived: the
                 * server answered early and handed the rest of the body to the
                 * parser as the next request.
                 *
                 * Dropping as we go also keeps the buffer a fixed window: a
                 * body of any size streams through it instead of needing the
                 * whole thing to fit, which is what used to end the connection
                 * with no response at all.
                 */
                size_t buffered = dx_buffer_readable(&conn->buf);
                size_t seen = (buffered > conn->body_offset) ? buffered - conn->body_offset : 0u;

                if (seen >= conn->body_left) {
                    /*
                     * Body complete. Drop exactly the declared body length:
                     * the buffer may already hold the next pipelined request
                     * behind it, and consuming "everything seen" would
                     * destroy that request. Then let dispatch build the
                     * response.
                     */
                    dx_buffer_consume(&conn->buf, conn->body_offset + (size_t)conn->body_left);
                    conn->body_left = 0;
                    conn->body_offset = 0;
                    if (conn_dispatch(w, conn) != 0) {
                        return -1;
                    }
                } else {
                    conn->body_left -= seen;
                    dx_buffer_consume(&conn->buf, buffered);
                    conn->body_offset = 0;
                    dx_conn_arm_timer(w, conn);
                    continue; /* wait for the rest of the body */
                }
            } else if (conn_process(w, conn) != 0) {
                return -1;
            }
            if (dx_conn_has_work(conn)) {
                /* Response staged, or a body still in flight: yield. */
                dx_conn_arm_timer(w, conn);
                return 0;
            }
            /*
             * A pipelined request restored by conn_after_response is already
             * sitting in the buffer (not in the socket), so reading more would
             * only hit EAGAIN. Parse it here: this is one level deep, not the
             * recursion the old tail call used to build.
             */
            if (conn->state == DX_CONN_READING && dx_conn_has_pending(conn)) {
                if (conn_process(w, conn) != 0) {
                    return -1;
                }
                if (dx_conn_has_work(conn)) {
                    dx_conn_arm_timer(w, conn);
                    return 0;
                }
                if (conn->state == DX_CONN_READING && dx_conn_has_pending(conn)) {
                    continue; /* more pipelined requests in the buffer */
                }
            }
            dx_conn_arm_timer(w, conn);
            return 0;
        }
        if (n == 0) {
            /*
             * EOF. A response already queued is still sent; a half delivered
             * request is dropped, because nothing completes it any more.
             * Keeping EPOLLIN registered here would busy loop, since read()
             * keeps returning 0 on a half closed socket.
             */
            conn->eof = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: read: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }

    if (conn->eof) {
        if (!dx_conn_has_work(conn)) {
            return -1;
        }
        dx_conn_arm_timer(w, conn);
        return 0; /* finish the reply through EPOLLOUT */
    }
    dx_conn_arm_timer(w, conn);
    return 0;
}
