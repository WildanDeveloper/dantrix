#include "conn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dantrix.h"
#include "net.h"
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
    dx_chunk_t **chunks;   /* base pointers, grows by doubling */
    size_t nchunks;
    size_t chunks_cap;
    size_t conn_slots;     /* slots per chunk */
    size_t max_conns;      /* hard cap of simultaneously held slots */
    uint32_t *free_list;   /* slot indices ready for reuse */
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
    pool->chunks[pool->nchunks++] = chunk;

    /* Every slot of the new chunk goes onto the free list. */
    for (i = 0; i < pool->conn_slots; i++) {
        size_t global_index = (pool->total_slots) + i;

        if (pool->free_count == pool->free_cap) {
            size_t new_cap = (pool->free_cap == 0) ? 64u : pool->free_cap * 2u;
            uint32_t *p = (uint32_t *)realloc(pool->free_list, new_cap * sizeof(*p));

            if (p == NULL) {
                /*
                 * Slots already registered stay usable; the rest of this chunk
                 * simply remains hidden until the next acquire attempt.
                 */
                pool->total_slots += i;
                return true;
            }
            pool->free_list = p;
            pool->free_cap = new_cap;
        }
        if (global_index > 0xffffffffu) {
            pool->total_slots += i;
            return true;
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
    if (pool->free_count < pool->free_cap) {
        pool->free_list[pool->free_count++] = conn->index;
    }
    /*
     * If the free list is full the slot is simply not reusable until the next
     * chunk allocation; it stays marked FREE so it can never be closed twice.
     */
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
/* Per-connection I/O                                                     */
/* --------------------------------------------------------------------- */

void dx_conn_init(dx_conn_t *conn, int fd)
{
    if (conn == NULL) {
        return;
    }
    conn->fd = fd;
    conn->state = DX_CONN_READING;
    conn->eof = false;
    conn->buf_mem = NULL;
    dx_buffer_init(&conn->buf, NULL, 0);
    conn->bytes_in = 0;
    conn->bytes_out = 0;
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

/*
 * Try to send everything pending. On EAGAIN the state moves to WRITING so the
 * worker registers EPOLLOUT; a successful drain moves it back to READING.
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
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG,
                     "conn %llu: send: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }
    conn->state = DX_CONN_READING;
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
    /* After an EOF, the echo is the last work this connection has to do. */
    if (conn->eof && !dx_conn_has_pending(conn)) {
        return -1;
    }
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

    while (budget > 0) {
        ssize_t n;

        budget--;

        /* Reclaim the consumed prefix so the whole capacity stays usable. */
        if (dx_buffer_writable(&conn->buf) == 0) {
            dx_buffer_compact(&conn->buf);
        }
        if (dx_buffer_writable(&conn->buf) == 0) {
            /*
             * Buffer full with unsent data: stop reading and let the writer
             * drain first. This is the backpressure path.
             */
            break;
        }

        n = read(conn->fd, conn->buf.data + conn->buf.len, dx_buffer_writable(&conn->buf));
        if (n > 0) {
            size_t got = (size_t)n;

            conn->buf.len += got;
            conn->bytes_in += got;
            continue;
        }
        if (n == 0) {
            /*
             * EOF. Data read before the FIN is still echoed back; only after
             * the echo has been flushed does the connection close. Keeping
             * EPOLLIN registered here would busy loop, because read() keeps
             * returning 0 on a half closed socket.
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
        dx_log_write(dx_worker_log(w), DX_LOG_DEBUG,
                     "conn %llu: read: %s",
                     (unsigned long long)conn->id, dx_strerror(errno));
        return -1;
    }

    if (conn_flush(w, conn) != 0) {
        return -1;
    }
    /*
     * EOF with a fully drained echo: nothing left to wait for. With output
     * still pending, conn_update_interest() keeps only EPOLLOUT registered and
     * the connection closes as soon as the flush completes.
     */
    if (conn->eof && !dx_conn_has_pending(conn)) {
        return -1;
    }
    return 0;
}
