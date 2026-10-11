# Changelog

All notable changes to Dantrix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project versions with
[Semantic Versioning](https://semver.org/). While the version is `0.x`, the
interfaces (CLI, configuration keys, HTTP behaviour) may change between minor
releases.

## [Unreleased]

### Added

- (this section collects changes since the last release)

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

[Unreleased]: https://github.com/WildanDeveloper/dantrix/compare/0.1.0...HEAD
[0.1.0]: https://github.com/WildanDeveloper/dantrix/releases/tag/0.1.0
