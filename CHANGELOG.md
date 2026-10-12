# Changelog

All notable changes to Dantrix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project versions with
[Semantic Versioning](https://semver.org/). While the version is `0.x`, the
interfaces (CLI, configuration keys, HTTP behaviour) may change between minor
releases.

## [Unreleased]

### Added

- (this section collects changes since the last release)

## [0.6.1] - 2026-10-12

Third audit round: 18 of 19 findings were real and are fixed here. The one
false positive (an empty `Location:` header on a truncated redirect) is
documented below, because proving it impossible is part of the value.

### Fixed

- **A directory listing read freed memory (heap-use-after-free, remote).**
  When a GET carried a `Content-Length` body that arrived in a later TCP
  segment, the header block was parked in `conn->hdr` and `base` pointed into
  it. The listing path called `dx_conn_release_scratch()` — which frees
  `conn->hdr` — and then `access_fill()` read the method, target, `Referer`
  and `User-Agent` through `base`. ASan reproduces it deterministically on
  `GET /subdir/` with a split body. Only the in-memory body is released now;
  the header block stays alive until the slot is recycled.
- **`pthread_mutex_destroy()` on an uninitialised mutex.** Several early
  failures in the multi-worker orchestrator (allocation, `pthread_sigmask`,
  `signalfd`, `bind`) jumped to the cleanup label before the reload snapshot
  was initialised, and the cleanup destroyed the mutex unconditionally. The
  mutex is now initialised before the first `goto out`, and the return of
  `pthread_mutex_init()` is checked.
- **A `416` response kept the connection alive.** `keep_alive` was computed
  from the status before the `Range` header was evaluated (so it assumed
  200/301), and the 416 branch used that value without setting
  `Connection: close`. Every other error path closes the connection; the 416
  now does too.
- **`-o key=value` overrides disappeared on `SIGHUP`.** The reload path
  replays the recorded command line, but only assignments made through the
  flag helpers were recorded — the `-o` case applied the value directly. The
  pair is recorded now, so an `-o` override survives a reload like any other
  flag.
- **The JSON status endpoint answered every method but OPTIONS.** A `POST` to
  the metrics path was served a 200 with the snapshot. The endpoint is
  read-only: `GET`/`HEAD` return 200 and every other method returns 405 with
  `Allow: GET, HEAD`.
- **`timegm()` conflated an error with 1969-12-31T23:59:59Z.** The parser
  rejected that legitimate instant because `timegm()` returns `(time_t)-1`
  for it (and the year guard rejected 1969 outright). The result is now
  round-tripped through `gmtime_r()` to tell the two apart.
- **A leap second was accepted.** The seconds bound was `value > 60`, which
  accepted `:60` while the in-code comment said leap seconds are refused. The
  bound is now `< 60`.
- **`active_connections` could underflow.** A close decremented the unsigned
  counter unconditionally; it is clamped at zero with a compare-exchange now.
- **`copy_bounded()` underflowed when `cap == 0`.** `cap - 1u` wraps to
  `SIZE_MAX`. No caller passes 0 today, but both copies of the helper guard
  it now.
- **`dx_daemonize()` used a single fork.** A session leader can reacquire a
  controlling terminal, so the process was not fully detached. The classic
  second fork is added; the intermediate child exits so the daemon is
  reparented to init.
- **`extra_len` could underflow the remaining header size.** `snprintf`
  returns the length it *would* have written, so a truncated append made
  `sizeof(extra) - extra_len` wrap. Every append is guarded now and the final
  check rejects the request instead of writing past the buffer.
- **`dx_server_run()` did not validate `access_log_path`**, which the log
  paths dereference unconditionally (`strcmp` against `"off"`). Both
  destinations are validated now.
- **Any duplicate `Content-Length` is refused**, including an identical one.
  RFC 9110 allows identical duplicates, but a duplicate framing header is what
  a smuggling proxy relies on and the server documents a strict posture.
  (This is a deliberate posture change, not a defect.)
- **Repository hygiene.** `public/index.html` is no longer swallowed by
  `.gitignore` (it ships as the example document root), the Dockerfile and the
  systemd unit now agree on `/var/www/dantrix`, the logrotate `postrotate`
  hook actually sends `SIGUSR1` (the stale comment claimed the feature did not
  exist yet), the systemd unit gained `ExecReload` and a trailing newline, and
  `conf/dantrix.conf` documents which values differ from the compiled
  defaults.

### Not a bug

- **An empty `Location:` header on a truncated redirect was reported but is
  unreachable.** `dx_fs_open()` encodes the path into a buffer of
  `DX_LOCATION_MAX` bytes with a cap of `DX_LOCATION_MAX - 2`, so the longest
  value `url_encode()` can return is `DX_LOCATION_MAX - 3`; the surrounding
  slashes bring the worst case to `DX_LOCATION_MAX - 1`, which still fits the
  caller's buffer. The `*location_len` guard is therefore never skipped.

### Remaining (documented, not fixed)

- `conn_serve_static()` and `dx_path_resolve()` together use roughly 23 KiB of
  stack per request. That is comfortable on the 8 MiB default stacks and the
  stacks are never reduced, but it is worth knowing before a port to a
  thread-pool design with smaller stacks.

### Added

- Regression checks for the four externally visible fixes (the listing with a
  drained body, the 416 close, the `-o`/SIGHUP interaction and the status
  endpoint methods), unit checks for the pre-epoch date and the counter clamp.

## [0.6.0] - 2026-10-12

Operations: configuration sources, signal driven lifecycle, privileges and a
JSON status endpoint.

### Added

- **Configuration file, environment and `-o` overrides.** `-c/--config` reads an
  INI style file, `DANTRIX_<KEY>` environment variables are honoured, and
  `-o key=value` overrides any key (repeatable). Priority, highest first: CLI
  flags and `-o`, then `DANTRIX_<KEY>`, then the file, then the built-in
  defaults. Every key is described exactly once in a registry, so the parser,
  `--print-config` and the defaults can never drift apart. Unknown keys,
  malformed values and out-of-range values are errors (exit code 2); a repeated
  single-value key warns and keeps the last value.
- **`-t/--test-config` and `--print-config`.** Validate (syntax plus the
  document root) or print the effective configuration, then exit, without
  starting the server.
- **Signal driven lifecycle via `signalfd`.** All control signals are blocked
  process-wide and read from a signalfd: no asynchronous handler ever runs.
  `SIGTERM`/`SIGINT` shut down gracefully, `SIGQUIT` closes immediately,
  `SIGHUP` reloads the reloadable keys, `SIGUSR1` reopens the log files (for
  `logrotate`), `SIGUSR2` writes a statistics snapshot. In multi-worker mode
  the main thread owns the signalfd and forwards commands to the workers
  through their eventfds.
- **Hot reload.** On `SIGHUP` the configuration sources are re-read and
  validated; on success every worker copies the reloadable subset (timeouts,
  HTTP limits, static options) into its own settings under a mutex. The hot
  path never touches the shared snapshot, so a reload costs one lock per worker
  and nothing per request. On a parse or validation error the previous settings
  are kept and the failure is logged.
- **Privilege drop, pid file and daemonize.** `--user`/`--group` drop privileges
  after `bind()` (`setgroups`, `setgid`, `setuid`, then verify `setuid(0)`
  fails). `--pid-file` writes and removes the pid file. `--daemon` forks into
  the background; the parent waits for the child to report readiness so a
  start-up failure still exits non-zero.
- **JSON status endpoint (`status_path` / `--status-path`).** Served only to
  loopback clients, it reports uptime, worker count, active/accepted/rejected
  connections, request totals, the response class breakdown and bytes sent.
  The counters are C11 `_Atomic`, incremented lock-free by the workers.
- New `src/config.c`, `src/process.c`, `src/status.c` with unit tests
  (`test_config.c`, `test_process.c`, `test_status.c`) and new regression
  checks for the signals, the reload and the status endpoint.

### Fixed

- **`signalfd` in the orchestrator blocked the wrong way.** A non-blocking
  signalfd read in the main thread returned `EAGAIN` immediately and left the
  server spinning without ever handling a signal; the orchestrator now creates
  a blocking signalfd, while the single-worker path keeps a non-blocking one
  registered in epoll.
- **A reload only reached one worker.** A shared "pending" flag was cleared by
  the first worker to apply the snapshot, so the others kept the old settings;
  each worker now copies the snapshot unconditionally when it receives RELOAD.
- **The command value was written on top of a wakeup byte.** The orchestrator
  wrote the generic "wakeup" (1) *and* the command bitmask into the same
  eventfd, so a `SIGUSR2` snapshot (value 4) also set the SHUTDOWN bit (5).
  Only the command value is written now.
- **The status counters depended on the access log.** They were updated inside
  the access-log emitter behind an early return, so a server with
  `access_log = off` reported zero requests; the counter update was moved out
  of that branch.

## [0.5.0] - 2026-10-12

Multi-core: one event loop per thread, no locks in the hot path.

### Added

- **Multi-worker `SO_REUSEPORT` (`-w/--workers N`).** `N > 1` spawns `N` worker
  threads, each with its own `epoll` instance, connection pool, buffer pool and
  timer wheel, and its own listener socket bound to the same address with
  `SO_REUSEPORT`; the kernel then load-balances new connections across the
  listeners by 4-tuple hash. Nothing is shared in the hot path, so there are no
  locks there.
- **Shared-listener fallback (`--reuseport off`).** All workers register one
  listener in their epoll with `EPOLLEXCLUSIVE` (Linux ≥ 4.5), so a new
  connection wakes exactly one worker. It is also selected automatically when
  the kernel rejects `SO_REUSEPORT`, and the listener is then owned by the
  orchestrator, which closes it exactly once.
- **`eventfd` shutdown channel.** The main thread no longer runs the event loop
  when `N > 1`: it blocks `SIGINT`/`SIGTERM` process-wide before the threads
  exist, waits in `sigwait()`, and on the stop signal writes to every worker's
  `eventfd`. A worker's wake-up handler drains the counter and enters the same
  graceful drain as the single-worker path. No state is written cross-thread
  and no code runs inside a signal handler.
- **Per-worker statistics.** `dx_worker_stats()` snapshots the accepted,
  rejected and closed counters; the orchestrator sums them after `join()` and
  logs the totals.
- **Optional CPU affinity (`--cpu-affinity on|off`).** Pins worker *i* to core
  *i*. A failure (fewer cores than workers, restricted mask) is logged as a
  warning and never fatal.
- **Thread-safe logging.** `dx_log_init()` attaches a mutex to a logger so
  error and access records from several workers cannot interleave mid-line;
  hand-built loggers (the unit tests) keep working unlocked.
- `tests/tsan_test.py`: drives concurrent traffic through the TSan build in
  both listener modes and fails on any ThreadSanitizer report; `make test-tsan`
  now builds **and runs** every unit test under TSan before running it.
- New `tests/unit/test_net.c` covering listener creation, `SO_REUSEPORT`
  sharing and the eventfd channel; new unit checks for CPU affinity and for
  concurrent log writes.

### Fixed

- **The CI workflow never ran since v0.4.1.** The build job's port
  expression was written as `${{ ${{ ... }} }}` (nested expressions), which
  GitHub rejects when parsing the workflow: every push was marked failed with
  *zero* jobs, so the "CI green" reported for v0.4.1 and v0.4.2 was never true.
  The expression is flattened; the four build cells and the sanitizers job now
  get their own test ports (they run in parallel and used to race for the same
  one), and the debug cells run the regression suite against
  `build/debug/dantrix` instead of the non-existent `bin/dantrix`.
- **`-w/--workers` never accepted its argument.** The short option `'w'` was
  declared without `:` in the getopt string, so `-w 4` left `optarg` NULL and
  the flag failed with "invalid --workers '(null)'" while `--workers 4` worked.
  The declaration now matches the usage text.

## [0.4.2] - 2026-10-12

Second audit round. Every defect was reproduced against the running server
before the fix and is now covered by a test (`test_timer.c`, and new checks in
`tests/smoke_test.py`).

### Fixed

- **File bodies were corrupted with `--sendfile off`.** The pread()+send()
  fallback advanced the file cursor only *after* a successful send, so an
  EAGAIN left the parked bytes in the buffer *and* the cursor unchanged: the
  next attempt read the same region again and duplicated it. The cursor now
  advances as soon as the bytes are copied out of the file, so a slow reader
  receives the file intact.
- **A sendfile body could overtake a partial response header.** `conn_flush_file()`
  wrote straight to the socket even when `conn_flush()` had left header bytes
  pending on EAGAIN. The sendfile path now returns until the header is fully
  out; the pread() path is inherently safe because it parks behind the header.
- **Directory listings carried two `Content-Length` headers.** The listing path
  added one and `dx_http_response_headers()` added another. Framing-header
  duplication is invalid (RFC 9110 8.6) and a request-smuggling primitive in
  front of a proxy.
- **`HEAD` on a listing reported `Content-Length: 0`.** It now reports the same
  length as the matching GET, as the README requires and the file path already
  did.
- **The access log wrote one line per connection, not per request.** The record
  was overwritten by each request and only emitted in `conn_close()`, so a
  keep-alive connection with four requests produced a single line for the last
  one. The line is now written as soon as each response is fully sent, and the
  record is cleared so a request is logged exactly once (a request that never
  reaches a response is still logged from `conn_close()`).
- **A lazy timer refresh could spin the event loop.** Raising the deadline of a
  bucket's minimum node inside the same second left the cached minimum pointing
  at the old, now past value, so `next_timeout_ms()` answered 0 and
  `epoll_wait(0)` spun at 100% CPU until the node was removed. The cache is
  recomputed when the minimum node moves later, matching `detach()`.
- **Listing entries with heavy escaping disappeared silently.** The HTML escape
  buffer was `NAME_MAX + 16` bytes while a single byte can expand to six
  (`&quot;`); a name of 240 `&` needed ~1440 bytes, `dx_html_escape()` returned
  0 and the entry was dropped. The buffer is now `NAME_MAX * 6 + 1`.
- **`dx_fs_open()` left `served_path` empty for redirects and listings**, so the
  caller had to fall back to another value. It is now filled for every
  non-error result and the contract is documented.
- **The connection generation could outgrow its tag field.** `dx_pool_acquire()`
  incremented a full 32-bit counter while the epoll tag stores only 30 bits
  (`DX_TAG_GEN_MASK`), so after 2^30 recycles every event for that slot would
  look stale. The counter now wraps at the mask.

### Changed

- `dx_worker_emit_access()` is public so `conn.c` can log a request when its
  response completes, instead of only at connection teardown.

## [0.4.1] - 2026-10-11

Audit release. Every defect below was reproduced before the fix and is now
covered by a test: the unit suite (regressions in `test_timer.c`, `test_pool.c`,
`test_mime.c`), `tests/smoke_test.py`, and the new `tests/regression_test.py`
for the cases that need their own server process.

### Fixed

- **Event loop could spin at 100% CPU forever.** The timing wheel cached the
  minimum deadline per bucket, but the lazy refresh path raises a node deadline
  without touching that cache. A removal then compared the raised deadline
  against the old minimum, decided the removed node was not the minimum and
  skipped the recompute, leaving a past deadline in an empty bucket.
  `next_timeout_ms()` answered `0` from then on and `epoll_wait(0)` never
  blocked. `detach()` now always recomputes.
- **Timer callbacks that mutate the wheel.** `advance()` followed a saved
  `next` pointer into a node that a callback had already detached, counting the
  active total down twice and refreshing the cached minimum of a bucket the
  node no longer belonged to. The bucket is now re-scanned after each firing.
- **Request bodies were double counted.** Every read recomputed the buffered
  body total and subtracted it again, so `body_left` fell faster than bytes
  arrived: the server answered before the body was complete and handed the rest
  to the parser as the next request (desynchronisation), and any body larger
  than the read buffer ended the connection with no response at all. Body bytes
  are now counted once and dropped as they arrive, with the header block moved
  into an owned copy, so a body of any size streams through a fixed window and
  a pipelined request behind it is still answered.
- **File bodies were truncated with `--sendfile off`.** The pread()+send()
  fallback read `dx_send_all()`'s return value as a byte count; it reports
  success as `0`, which the loop read as "file ended early", so every file
  stopped after the first 16 KiB chunk. The fallback now streams through the
  connection buffer, which also makes a partial send resumable.
- **Large directory listings answered nothing.** A listing that did not fit
  the 2 KiB staging buffer could not be turned into a response, so the
  connection was closed with zero bytes. The body is rendered into an owned
  buffer and streamed like a file body.
- **Rejected connections leaked a descriptor.** Both rejection paths in
  `conn_open()` returned without closing the descriptor `accept4()` had just
  handed over: one leak per refused client, which exhausts the process under
  load.
- **Pipelined requests were dropped on most response paths.** Only the file
  path consumed the request bytes and parked the bytes that followed, so a
  redirect, a listing, `HEAD`, `304` or `416` overwrote them with the response.
  `conn_finish()` is now the single place that drops the request, parks the
  pipelined bytes and stages the response.
- **Graceful shutdown skipped live connections.** Connection slots are handed
  out from the top of the free list, so with `--max-conns 2` the live
  connections sit at indices 1023 and 1022 while the drain loop scanned
  `[0, max_conns)`: nothing was closed gracefully and every access log line was
  lost. The loop now walks the pool's real slot space.
- **Reflected XSS through the listing title.** The requested path is attacker
  controlled and was written into `<title>` and `<h1>` unescaped; a directory
  named `<img src=x onerror=...>` became markup. It is HTML escaped now.
- **`Location` was built from the decoded path.** A redirect could emit a raw
  space or a `#`, which is not a valid URI-reference, and double decoding
  pointed at a different resource. The path is percent encoded again.
- **Heap buffer overflow loading a large `mime.types`.** The reader allocated
  exactly the bytes it read and then terminated the last line one byte past the
  end when the file reached the 1 MiB cap. The allocation carries one spare
  byte and the terminator is only written inside it.
- **`--server-tokens off` was ignored.** The flag was parsed and never read;
  responses always carried `Server: Dantrix/x.y.z`. The setting now reaches the
  response builder.
- **`304` responses carried `Content-Length: 0`**, which told a client the
  cached representation was empty. `304` and `204` carry no `Content-Length` at
  all now.
- **A recycled connection slot kept the previous owner's state.** Reset only
  cleared part of the slot, and never freed the read buffer that had outgrown
  its pooled slot, the parked pipelined bytes or the scratch allocations.
  `dx_pool_release()` now resets the slot, so the invariant holds on every
  path. Reset also stopped closing a stale `file_fd`: descriptor numbers are
  reused, and closing one that belonged to the previous owner closed the *next*
  connection's descriptor (`epoll_ctl(ADD): Bad file descriptor`).
- **A `408` could be truncated.** The result of the write was ignored and the
  connection was closed immediately; on `EAGAIN` the client received a partial
  error page. The pending response is now finished through the writable path,
  bounded by the send timeout.
- **Descriptor exhaustion stalled the event loop.** `accept_loop()` slept
  100 ms inside the loop on `EMFILE`, stalling every other connection. The
  listener is now removed from the epoll set and the retry is scheduled on the
  timer wheel, so there is neither a spin nor a sleep.
- **`accept4()` peer address was read uninitialised**, and `conn.hold` was
  parsed as if it held body bytes.
- Uninitialised `sockaddr_storage`, a duplicated `--root` option, a signed
  comparison against `0x20` in the fuzz harness, a `snprintf()` result cast to
  `size_t` without a negative check, a silent MIME truncation, pointer
  comparison across distinct allocations in the buffer pool, a stale
  `content_type` read by the path fuzz harness, an unused `dx_sockerr()`, a
  `417` reason phrase the server never emits, and `write()` returning `0`
  spinning forever in the teaching examples.

### Changed

- `make` now tracks header dependencies (`-MMD -MP`). A changed header used to
  leave stale objects behind, and the mismatched struct layouts surfaced as
  wrong counters at runtime instead of a rebuild.
- `make test-tsan` builds every test against the whole library (the target
  referenced a doubled path and could not run at all); `VERSION` in the
  `Makefile` was `0.1.0`.
- The test scripts take their port from `DANTRIX_TEST_PORT` /
  `DANTRIX_C10K_PORT` so two suites can run at the same time.
- `deploy/` ships the files the README referenced: `dantrix.service`,
  `dantrix.logrotate`, `sysctl-dantrix.conf`, `Dockerfile`,
  `docker-compose.yml`. The `logrotate` hook does not send `SIGUSR1` yet:
  that signal is a v0.6 feature and its default disposition would kill the
  process.
- Documentation now matches the code: the listing and module sections name the
  files that exist, the log section states that records are written
  synchronously and that reopening is planned, and `conf/dantrix.conf` no
  longer advertises the `json` log format.
- The server prints which path resolution the kernel provides at start-up
  (`openat2(RESOLVE_BENEATH)` or the manual walk), which the unused
  `dx_fs_has_openat2()` probe already knew.

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
