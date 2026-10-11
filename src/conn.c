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
     * Reset per-field, never memset: the generation counter must survive the
     * recycle so that events stamped with the previous generation are
     * recognised as stale instead of resolving to this new owner.
     */
    conn->fd = -1;
    conn->state = DX_CONN_READING;
    conn->eof = false;
    conn->buf_mem = NULL;
    dx_buffer_init(&conn->buf, NULL, 0);
    conn->bytes_in = 0;
    conn->bytes_out = 0;
    conn->index = index; /* constant per slot; re-set defensively */

    /* Skip generation 0 so a zeroed tag can never look valid. */
    if (conn->generation == 0xffffffffu) {
        conn->generation = 0u;
    }
    conn->generation++;
    conn->id = ++pool->next_id;
    pool->active++;
    return conn;
}

bool dx_pool_release(dx_pool_t *pool, dx_conn_t *conn)
{
    if (pool == NULL || conn == NULL) {
        return false;
    }
    if (conn->state == DX_CONN_FREE) {
        return false; /* double release */
    }

    conn->state = DX_CONN_FREE;
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

size_t dx_pool_active(const dx_pool_t *pool)
{
    return (pool == NULL) ? 0u : pool->active;
}

/* --------------------------------------------------------------------- */
/* Per-connection HTTP flow                                               */
/* --------------------------------------------------------------------- */

/* Stack staging area for one response; 2 KiB covers every v0.2 response.
 * File responses append validators on top, so the buffer is sized for that. */
#define DX_RESPONSE_TMP 2048u

/* Scratch for the pread()+send() fallback of sendfile(). */
#define DX_FILE_SCRATCH 16384u

/* Buffer for a generated directory listing. */
#define DX_LISTING_TMP 32768u

/* The single body v0.2 serves when no document root is configured. */
static const char conn_hello_body[] = "Dantrix " DX_VERSION_STRING ": the HTTP core is alive.\n";

void dx_conn_init(dx_conn_t *conn, int fd, const dx_http_limits_t *limits)
{
    if (conn == NULL) {
        return;
    }
    conn->fd = fd;
    conn->state = DX_CONN_READING;
    conn->eof = false;
    conn->close_after_response = false;
    conn->buf_mem = NULL;
    conn->file_fd = -1;
    conn->file_offset = 0;
    conn->file_left = 0;
    dx_buffer_init(&conn->buf, NULL, 0);
    dx_http_parser_init(&conn->parser, limits);
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

void dx_conn_reset(dx_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    conn->fd = -1;
    conn->buf_mem = NULL;
    dx_buffer_detach(&conn->buf);
}

bool dx_conn_has_pending(const dx_conn_t *conn)
{
    return (conn != NULL) && dx_buffer_readable(&conn->buf) > 0;
}

bool dx_conn_has_work(const dx_conn_t *conn)
{
    return (conn != NULL) && (dx_buffer_readable(&conn->buf) > 0 || conn->file_left > 0);
}

/*
 * Try to send everything pending. On EAGAIN the state moves to WRITING so the
 * worker registers EPOLLOUT. A full drain returns -1 when the connection must
 * close right away: v0.2 closes after every response, and after a peer EOF
 * there is nothing left to wait for either.
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
            return 0;
        }
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG, "conn %llu: send: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }
    conn->state = DX_CONN_READING;
    /* A streamed file counts as pending output: the connection stays open
     * until the last body byte is on the wire. */
    if ((conn->close_after_response && conn->file_left == 0) || conn->eof) {
        return -1;
    }
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

    while (conn->file_left > 0) {
        size_t chunk = conn->file_left;
        ssize_t moved;

        if (chunk > w->static_opts.sendfile_chunk) {
            chunk = w->static_opts.sendfile_chunk;
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
                moved = pread(conn->file_fd, scratch,
                              (chunk > sizeof(scratch)) ? sizeof(scratch) : chunk,
                              (off_t)conn->file_offset);
                if (moved > 0) {
                    moved = dx_send_all(conn->fd, scratch, (size_t)moved);
                }
            }
        } else {
            moved =
                pread(conn->file_fd, scratch, (chunk > sizeof(scratch)) ? sizeof(scratch) : chunk,
                      (off_t)conn->file_offset);
            if (moved > 0) {
                moved = dx_send_all(conn->fd, scratch, (size_t)moved);
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
    if (conn->close_after_response && conn->file_left == 0) {
        return -1;
    }
    return 0;
}

int dx_conn_on_writable(struct dx_worker *w, dx_conn_t *conn)
{
    if (conn == NULL || conn->state == DX_CONN_FREE || conn->state == DX_CONN_CLOSING) {
        return 0;
    }
    if (conn_flush(w, conn) != 0) {
        return -1;
    }
    /* Header buffer drained: continue the file body, if one is armed. */
    if (conn->file_left > 0 && conn_flush_file(w, conn) != 0) {
        return -1;
    }
    return 0;
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
 * bytes first and v0.2 closes after every response, so overwriting the buffer
 * is safe: nothing else shares the allocation at this point.
 */
static int conn_stage_response(dx_conn_t *conn, const char *resp, size_t len)
{
    if (conn->buf.data == NULL || len > conn->buf.cap) {
        return -1;
    }
    memcpy(conn->buf.data, resp, len);
    conn->buf.off = 0;
    conn->buf.len = len;
    conn->close_after_response = true;
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
 * -1 on a fatal error. Every response path sets conn->close_after_response
 * because v0.3 still closes after every response.
 */
static int conn_serve_static(struct dx_worker *w, dx_conn_t *conn, const dx_http_request_t *req,
                             const unsigned char *base, bool head)
{
    const char *target = (const char *)base + req->target_off;
    size_t target_len = req->target_len;
    char tmp[DX_RESPONSE_TMP];
    char listing[DX_LISTING_TMP];
    char extra[768];
    char content_type[160];
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
    size_t voff;
    size_t vlen;

    /* 1. Target to filesystem path. */
    path_opts.hidden_files = w->static_opts.hidden_files;
    status = dx_path_resolve(target, target_len, &path_opts, &resolved);
    if (status != 0) {
        len = dx_http_response_error(tmp, sizeof(tmp), status, NULL);
        access_fill(conn, req, base, status, 0);
        if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        return conn_flush(w, conn);
    }

    if (w->fs == NULL) {
        status = 404;
        len = dx_http_response_error(tmp, sizeof(tmp), status, NULL);
        access_fill(conn, req, base, status, 0);
        if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        return conn_flush(w, conn);
    }

    /* 2. Open below the document root. */
    open_result =
        dx_fs_open(w->fs, resolved.path, resolved.had_trailing_slash, location, sizeof(location),
                   &location_len, served_file, sizeof(served_file), &opened);

    if (open_result == DX_OPEN_REDIRECT) {
        extra_len = (size_t)snprintf(extra, sizeof(extra), "Location: %.*s\r\n", (int)location_len,
                                     location);
        status = 301;
        len = dx_http_response_build(tmp, sizeof(tmp), status, extra, NULL, NULL, 0u, true);
        access_fill(conn, req, base, status, 0);
        if (len == 0 || extra_len >= sizeof(extra) || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        return conn_flush(w, conn);
    }

    if (open_result == DX_OPEN_LISTING) {
        size_t body_len = dx_fs_list(w->fs, resolved.path, listing, sizeof(listing));

        if (body_len == 0) {
            status = 403; /* unreadable or too large to render */
            len = dx_http_response_error(tmp, sizeof(tmp), status, NULL);
            access_fill(conn, req, base, status, 0);
            if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
                return -1;
            }
            return conn_flush(w, conn);
        }
        len = dx_http_response_build(tmp, sizeof(tmp), 200, NULL, "text/html; charset=utf-8",
                                     listing, body_len, head);
        access_fill(conn, req, base, 200, head ? 0u : body_len);
        if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        return conn_flush(w, conn);
    }

    if (open_result != DX_OPEN_OK) {
        status =
            (open_result == DX_OPEN_FORBIDDEN) ? 403 : ((open_result == DX_OPEN_ERROR) ? 500 : 404);
        len = dx_http_response_error(tmp, sizeof(tmp), status, NULL);
        access_fill(conn, req, base, status, 0);
        if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        return conn_flush(w, conn);
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
            extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                          "ETag: %s\r\n", etag);
        }
        if (w->static_opts.cache_control != NULL && w->static_opts.cache_control[0] != '\0') {
            extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                          "Cache-Control: %s\r\n", w->static_opts.cache_control);
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
            len = dx_http_response_build(tmp, sizeof(tmp), 304, extra, NULL, NULL, 0u, true);
            access_fill(conn, req, base, 304, 0);
            if (len == 0 || extra_len >= sizeof(extra) ||
                conn_stage_response(conn, tmp, len) != 0) {
                return -1;
            }
            (void)close(opened.fd);
            return conn_flush(w, conn);
        }

        /* 4. Range handling (GET only; HEAD always describes the full file). */
        {
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
                int written = snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                       "Content-Range: bytes */%llu\r\nAccept-Ranges: bytes\r\n",
                                       (unsigned long long)size);

                len = dx_http_response_build(tmp, sizeof(tmp), 416, extra, NULL, NULL, 0u, true);
                access_fill(conn, req, base, 416, 0);
                if (len == 0 || written < 0 || (size_t)written >= sizeof(extra) - extra_len ||
                    conn_stage_response(conn, tmp, len) != 0) {
                    (void)close(opened.fd);
                    return -1;
                }
                (void)close(opened.fd);
                return conn_flush(w, conn);
            }

            if (range_result == DX_RANGE_OK) {
                start = range.start;
                body_len = range.length;
                partial = true;
                extra_len += (size_t)snprintf(
                    extra + extra_len, sizeof(extra) - extra_len,
                    "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)range.start,
                    (unsigned long long)range.end, (unsigned long long)size);
            }
            extra_len += (size_t)snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                          "Accept-Ranges: bytes\r\n");

            (void)dx_mime_content_type(w->mime,
                                       (served_file[0] != '\0') ? served_file : resolved.path,
                                       content_type, sizeof(content_type));

            status = partial ? 206 : 200;
            len = dx_http_response_headers(tmp, sizeof(tmp), status, extra, content_type,
                                           (size_t)body_len, head);
            access_fill(conn, req, base, status, head ? 0u : (size_t)body_len);
            if (len == 0 || extra_len >= sizeof(extra) ||
                conn_stage_response(conn, tmp, len) != 0) {
                (void)close(opened.fd);
                return -1;
            }

            if (head) {
                /* HEAD sends identical headers and no body. */
                (void)close(opened.fd);
                return conn_flush(w, conn);
            }
            /* Arm the transfer; the body is streamed after the headers. */
            conn->file_fd = opened.fd;
            conn->file_offset = start;
            conn->file_left = body_len;
        }
    }

    if (conn_flush(w, conn) != 0) {
        return -1;
    }
    if (conn->file_left > 0 && conn_flush_file(w, conn) != 0) {
        return -1;
    }
    return 0;
}

/*
 * Feed the parser and, when a request is complete (or rejected), build and
 * queue the response. Returns 0 while the connection stays open, -1 when it
 * must close now.
 */
static int conn_process(struct dx_worker *w, dx_conn_t *conn)
{
    size_t consumed = 0;
    dx_parse_result_t r;
    char tmp[DX_RESPONSE_TMP];
    size_t len;
    int status;
    const char *extra = NULL;
    size_t body_bytes = 0;
    const unsigned char *base;
    const dx_http_request_t *req;

    if (conn->buf.data == NULL) {
        return -1; /* unreachable: every connection owns a buffer */
    }
    r = dx_http_parser_feed(&conn->parser, conn->buf.data + conn->buf.off,
                            dx_buffer_readable(&conn->buf), &consumed);
    if (r == DX_PARSE_NEED_DATA) {
        return 0; /* wait for more bytes */
    }

    if (r == DX_PARSE_ERROR) {
        /* The request is rejected before any method is known. */
        status = conn->parser.status_code;
        len = dx_http_response_error(tmp, sizeof(tmp), status, NULL);
        access_fill(conn, NULL, NULL, status, 0);
        if (len == 0 || conn_stage_response(conn, tmp, len) != 0) {
            return -1;
        }
        /*
         * The client may still be sending the oversized or malformed
         * request; discard what is already in flight so the error response
         * is read instead of lost to an RST.
         */
        conn_discard_input(conn);
        return conn_flush(w, conn);
    }

    base = conn->buf.data + conn->buf.off;
    req = &conn->parser.req;

    switch (dx_http_dispatch_method(req, base, &status, &extra)) {
    case DX_DISPATCH_SERVE:
        status = 200;
        if (method_is(req, base, "OPTIONS")) {
            extra = "Allow: GET, HEAD, OPTIONS\r\n";
            len = dx_http_response_build(tmp, sizeof(tmp), 200, extra, NULL, NULL, 0u, false);
        } else {
            bool head = method_is(req, base, "HEAD");

            if (w->fs != NULL) {
                /* Static file serving: the response may arm a file transfer,
                 * which continues in conn_flush_file() after the headers.
                 * The request bytes are not consumed yet: the resolver and
                 * the header lookups still read them through base. */
                return conn_serve_static(w, conn, req, base, head);
            }
            /* No document root configured: the core still answers. */
            len = dx_http_response_build(tmp, sizeof(tmp), 200, NULL, "text/plain; charset=utf-8",
                                         conn_hello_body, sizeof(conn_hello_body) - 1u, head);
            body_bytes = head ? 0u : sizeof(conn_hello_body) - 1u;
        }
        break;
    case DX_DISPATCH_ERROR:
    default:
        len = dx_http_response_error(tmp, sizeof(tmp), status, extra);
        break;
    }

    if (len == 0) {
        dx_log_write(dx_worker_log(w), DX_LOG_ERROR,
                     "conn %llu: response did not fit the staging buffer",
                     (unsigned long long)conn->id);
        return -1;
    }

    /* Log fields must be captured while the request bytes are still valid. */
    access_fill(conn, req, base, status, body_bytes);

    dx_buffer_consume(&conn->buf, consumed);
    if (conn_stage_response(conn, tmp, len) != 0) {
        return -1;
    }
    return conn_flush(w, conn);
}

/*
 * Double the read buffer for requests larger than the pooled slot size. Growth
 * is bounded by the configured header limit, so a client can never make the
 * server allocate without end. The live bytes move to a fresh allocation with
 * the request start at offset 0, which keeps every parser offset valid.
 * Returns 0 on success, -1 when the limit is reached or malloc fails.
 */
static int conn_grow(struct dx_worker *w, dx_conn_t *conn)
{
    size_t cap_max = w->limits.max_header_size + 4u;
    size_t want;
    size_t live = dx_buffer_readable(&conn->buf);
    unsigned char *fresh;

    if (conn->buf.cap >= cap_max) {
        return -1; /* limit reached: the parser answers 431 before this point */
    }
    want = conn->buf.cap * 2u;
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
    if (conn->close_after_response) {
        /*
         * The response is queued or on the wire; whatever else the client
         * sends is irrelevant because the connection closes after the flush.
         * Interest no longer includes EPOLLIN, so this guard only covers the
         * window between staging the response and updating the interest.
         */
        return 0;
    }

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
            size_t got = (size_t)n;

            conn->buf.len += got;
            conn->bytes_in += got;
            if (conn_process(w, conn) != 0) {
                return -1; /* response sent or queued: the flush decided */
            }
            if (conn->close_after_response) {
                return dx_conn_has_work(conn) ? 0 : -1;
            }
            continue;
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
        return 0; /* finish the reply through EPOLLOUT */
    }
    return 0;
}
