# Changelog

All notable changes to Dantrix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project versions with
[Semantic Versioning](https://semver.org/). While the version is `0.x`, the
interfaces (CLI, configuration keys, HTTP behaviour) may change between minor
releases.

## [Unreleased]

### Added

- (this section collects changes since the last release)

## [0.4.0] - 2026-10-11

### Added

- `src/timer.c`: hashed timing wheel with one intrusive node per connection
  (256 buckets of one second). Installing, refreshing and cancelling are O(1),
  a refresh inside the same second keeps the node where it is (lazy refresh),
  and `dx_timer_wheel_next_timeout_ms()` gives the event loop the exact
  millisecond timeout for `epoll_wait`. Deadlines are absolute, so a timer in a
  shared bucket is never fired early.
- Keep-alive: responses no longer close the connection. The connection
  survives when the status is below 400, the client did not ask for `close`,
  HTTP/1.1 is in use (or HTTP/1.0 sent `Connection: keep-alive`) and the
  `keepalive_requests` budget is not exhausted; otherwise the response carries
  `Connection: close` and the socket ends after it.
- Pipelining: requests already buffered are answered in order after the
  previous response has been fully sent. Bytes that follow a request are
  parked in a small holdover while the response reuses the read buffer.
- Request bodies are read and discarded (bounded by `max_body_size`, `413`
  above it) so a kept-alive connection stays aligned; `body_timeout` guards the
  drain.
- Timeouts wired to the wheel: `header_timeout` (with a `408` answer when part
  of a request arrived, a silent close when nothing did), `body_timeout`,
  `send_timeout` (refreshed on every write progress step) and
  `keepalive_timeout`.
- Idle connections release their read buffer back to the pool, which is what
  keeps 10,000 idle connections cheap; the buffer is taken again when the peer
  sends data.
- Graceful shutdown: the listener is detached, in-flight connections finish,
  and whatever is left is closed when `shutdown_timeout` expires.
- New CLI flags: `--keepalive-requests`, `--header-timeout`, `--body-timeout`,
  `--send-timeout`, `--keepalive-timeout`, `--shutdown-timeout` (durations
  accept `s`, `m` and `ms` suffixes) and `--max-body-size`.
- `tests/c10k_test.py` (`make test-c10k N=...`): holds N idle connections
  open, checks every one of them still answers, and verifies a clean shutdown.
  10,000 connections are accepted in well under a second.
- Tests: `test_timer` (80 checks) and a smoke test extended to 65 checks
  covering keep-alive, pipelining, body draining, `408`, idle closes and the
  access log for repeated requests.

### Fixed

- A pending response is now tracked explicitly instead of inferred from bytes
  in the buffer: request bytes waiting to be parsed are input, not output, and
  treating them as pending output made the server answer partial requests by
  writing them back to the client.
- The `408` written on a header timeout no longer includes the partially
  received request in the response.
- A request body that arrived together with the headers, or split across
  packets, is drained exactly once; before, the body bytes were consumed twice
  and the next request on the connection was parsed from a shifted offset.

## [0.3.0] - 2026-10-11

### Added

- `src/path.c`: request target to filesystem path resolution following the
  README order: query stripping, absolute-form authority removal, strict
  validation, **percent-decoding exactly once**, normalization (`//` collapse,
  `.` drop, `..` resolution with a `400` on escape), and hidden segment refusal
  (`404`, `/.well-known/` exempt). Decoded bytes are re-validated, so `%0a`
  and `%00` cannot smuggle control characters into paths, logs or listings.
- `src/file.c`: document root access through `openat2(RESOLVE_BENEATH)` with
  `RESOLVE_NO_SYMLINKS` when symlinks are refused, and a per-segment
  `O_NOFOLLOW` fallback for kernels without `openat2`. Directory handling:
  `301` to add the trailing slash, index file serving, optional HTML listing
  with escaped names and percent-encoded hrefs, `403` without an index,
  `404` for non-regular files. Symlinks that would leave the root are refused
  even with `follow_symlinks = on`.
- `src/http_range.c`: single-range parsing (`bytes=0-499`, `bytes=500-`,
  `bytes=-500`) with `206`, `416` (`Content-Range: bytes */size`), multi-range
  deliberately ignored (full `200`, closing the overlapping-range attack
  class), nginx-style `ETag` formatting (`"<mtime-hex>-<size-hex>"`), weak
  `If-None-Match` comparison (`*` and lists), strict `If-Modified-Since`
  IMF-fixdate parsing and strong-only `If-Range`.
- `src/mime.c`: built-in table of ~70 extensions with the charset rule (every
  text type, `application/json` and JavaScript get `; charset=utf-8`), plus a
  custom `mime.types` file parser that overrides built-ins.
- File responses in the connection flow: `Last-Modified`, `ETag`,
  `Accept-Ranges: bytes`, `Cache-Control`, `206`/`304`/`416` handling, and
  zero-copy delivery with `sendfile()` and a `pread()`+`send()` fallback.
- New CLI flags: `--root`, `--index`, `--directory-listing`, `--sendfile`,
  `--sendfile-chunk`, `--etag`, `--follow-symlinks`, `--hidden-files`,
  `--mime-types`, `--default-type`, `--cache-control`, `--server-tokens`.
- `fuzz/fuzz_path.c`: fuzz harness for path resolution, MIME mapping and the
  header-driven conditional/range logic, asserting that a resolved path is
  never absolute, never contains `..` and never carries control bytes.
- Tests: `test_path` (76 checks), `test_mime` (63), `test_http_range` (112),
  `test_file` (49, against a real temporary document root), plus a rewritten
  smoke test covering the whole file surface (48 integration checks).

### Fixed

- Root path handling: `GET /` no longer redirects to `/./`, and the MIME type
  is derived from the file actually served (the index) instead of the
  directory path.
- The response builder no longer rejects a NULL body with a non-zero length,
  which silently produced empty responses for every file request.
- `join_path()` returning a length instead of a success code meant directory
  index files were never opened.

## [0.2.0] - 2026-10-11

### Added

- `src/http_parser.c`: incremental, zero-copy HTTP/1.x request line and header
  parser. A request may arrive in any number of pieces; parsed fields are
  offsets into the caller's buffer. Strict rules per the README: `CRLF` only
  (bare `LF` or bare `CR` rejected), no whitespace before the colon, no
  obs-fold, token-validated methods and header names, control bytes and DEL
  rejected in targets and values.
- Header semantics: `Host` required (and unique, non-empty) in HTTP/1.1,
  `Content-Length` must be decimal with conflicting duplicates rejected,
  `Transfer-Encoding` answers `501` and `501` becomes `400` when combined with
  `Content-Length`, unknown HTTP versions answer `505`.
- Configurable limits with `414`/`431` enforcement: `--max-request-line`,
  `--max-header-size` (accepts `k`/`m`/`g` suffixes) and `--max-headers`.
- `src/http_response.c`: bounded-append response builder (no `sprintf` into
  header buffers), fixed error pages that never reflect client input, `Date`
  IMF-fixdate header, `Connection: close` on every response, `HEAD` semantics
  (identical headers, no body) and `OPTIONS` with `Allow`.
- Method dispatch: `GET`, `HEAD`, `OPTIONS` served with a static `200`;
  `POST`/`PUT`/`DELETE`/`PATCH`/`TRACE`/`CONNECT` answer `405` with `Allow`;
  unknown methods answer `501`. Method names are case sensitive.
- Access log (`--access-log`, `--log-format common|combined`): one CLF line per
  completed request with the peer address captured at `accept4` time. All
  client-controlled fields are escaped (`\xHH`) so quotes, backslashes and
  control characters cannot forge log lines.
- New CLI flags: `--access-log DEST`, `--log-format FMT`,
  `--max-request-line N`, `--max-header-size N`, `--max-headers N`.
- Read buffers grow on demand from the pooled 4 KiB slot up to
  `max_header_size + 4`, bounded, so oversized requests are answered with
  `431` instead of truncating the parser's view.
- After a protocol error the server discards in-flight input (bounded to
  64 KiB) before closing, so the error response is read by the client instead
  of being destroyed by a TCP reset.
- `src/util.c`: `dx_parse_ull` (strict decimal over an exact byte range),
  `dx_parse_size` (binary `k`/`m`/`g` suffixes), `dx_http_date` (IMF-fixdate).
- Tests: `tests/unit/test_http_parser.c` (166 checks: table-driven malformed
  cases, byte-by-byte and CRLF-boundary split feeds, limit boundaries),
  `tests/unit/test_http_response.c` (51 checks), access log and util coverage
  (115 additional checks), and `fuzz/fuzz_http_parser.c`, a parser fuzz harness
  with a deterministic standalone driver (300k mutated inputs per run,
  libFuzzer-compatible). The smoke test now exercises the HTTP surface end to
  end (45 integration checks).

### Fixed

- Access log: structural quotes were escaped like payload data, and the
  timestamp prefix contained an escaped NUL byte from a wrong literal length.

## [0.1.0] - 2026-10-11

### Added

- Project skeleton: `Makefile` with release, debug, profile, sanitizer, test,
  lint, format and install targets; `.clang-format`, `.editorconfig`,
  `.gitignore`, CI workflow.
- Learning examples, one self-contained file per step:
  `examples/01_echo_blocking.c`, `02_echo_thread_per_conn.c`,
  `03_echo_epoll.c`, `04_hello_http.c`.
- `src/net.c`: TCP listener helper with `SOCK_NONBLOCK | SOCK_CLOEXEC`,
  `SO_REUSEADDR`, dual stack IPv6, plus `TCP_NODELAY` and non-blocking helpers.
- `src/worker.c`: single threaded, level-triggered `epoll` event loop with an
  accept loop, bounded batches, stale event rejection through slot generations,
  and backpressure via `EPOLLOUT` registration only while output is pending.
- `src/conn.c`: connection slot pool (slab chunks, free list, generation
  counters), connection state, echo I/O with partial write and EOF handling.
- `src/buffer.c`: growable byte buffer window and a fixed size buffer pool with
  ownership bitmaps that reject double releases.
- `src/log.c`: leveled logger (debug, info, warn, error) writing to stderr,
  stdout or a file.
- `src/server.c` and `src/main.c`: signal handling (`SIGINT`/`SIGTERM` graceful
  shutdown, `SIGPIPE` ignored), listener creation, command line parsing with
  exit codes 0/1/2.
- Build hardening: PIE, RELRO, stack protector, stack clash protection,
  `_FORTIFY_SOURCE`, strict warning set with `-Werror`.
- Tests: unit tests for buffer, pool, log and util modules (179 checks), plus a
  sanitizer-friendly integration smoke test (`tests/smoke_test.py`).

[Unreleased]: https://github.com/WildanDeveloper/dantrix/compare/0.4.0...HEAD
[0.4.0]: https://github.com/WildanDeveloper/dantrix/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/WildanDeveloper/dantrix/compare/0.2.0...0.3.0
[0.2.0]: https://github.com/WildanDeveloper/dantrix/compare/0.1.0...0.2.0
[0.1.0]: https://github.com/WildanDeveloper/dantrix/releases/tag/0.1.0
