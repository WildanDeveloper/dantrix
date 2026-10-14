/*
 * dantrix.h - Version, compile-time constants and limits shared by all modules.
 *
 * Everything here is a constant or a macro: this header must never depend on
 * another project header so that it can be included from any translation unit.
 */
#ifndef DANTRIX_H
#define DANTRIX_H

/* Version. Keep in sync with CHANGELOG.md and the README status table. */
#define DX_VERSION_MAJOR 0
#define DX_VERSION_MINOR 8
#define DX_VERSION_PATCH 8
#define DX_VERSION_STRING "0.8.8"
#define DX_PROGRAM_NAME "Dantrix"

/*
 * Maximum number of events returned by a single epoll_wait() call. Bounding
 * the batch keeps a burst of activity from starving timers and log flushing in
 * implementations that add them.
 */
#define DX_MAX_EVENTS 1024

/* Connection objects carved out of a single allocation ("slab chunk"). */
#define DX_CONN_CHUNK_SLOTS 1024u

/* Initial read buffer size for a new connection. Grows later in the HTTP work. */
#define DX_CONN_BUF_SIZE 4096u

/* Read buffers carved out of a single allocation in the buffer pool. */
#define DX_BUF_CHUNK_SLOTS 256u

/* Defaults used when the corresponding command line flag is absent. */
#define DX_DEFAULT_BIND "0.0.0.0"
#define DX_DEFAULT_PORT 8080
#define DX_DEFAULT_BACKLOG 4096
#define DX_DEFAULT_MAX_CONNS 16384
#define DX_DEFAULT_ROOT "./public"
#define DX_DEFAULT_INDEX "index.html"
#define DX_DEFAULT_MIME "application/octet-stream"

/*
 * Event tag layout used as epoll_event.data.u64:
 *
 *   bits 63..62 : kind (dx_tag_kind_t)
 *   bits 61..32 : connection slot generation
 *   bits 31..0  : connection slot index
 *
 * A tag never carries a raw pointer: when a slot has been recycled, the
 * generation carried by a stale event no longer matches and the event is
 * dropped instead of dereferencing memory that now belongs to another
 * connection.
 */
#define DX_TAG_KIND_SHIFT 62u
#define DX_TAG_GEN_SHIFT 32u
#define DX_TAG_INDEX_MASK 0xffffffffu
#define DX_TAG_GEN_MASK 0x3fffffffu

#endif /* DANTRIX_H */
