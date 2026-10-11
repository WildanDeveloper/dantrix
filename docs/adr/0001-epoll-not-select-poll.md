# ADR 0001: epoll instead of select or poll

- Status: accepted
- Date: 2026-10-11
- Deciders: project maintainer

## Context

Dantrix must watch tens of thousands of descriptors from one thread. The three
readiness APIs available on Linux are `select`, `poll` and `epoll`.

`select` scales with `FD_SETSIZE` (typically 1024) and rescans the whole set on
every call. `poll` lifts the descriptor limit but still scans every descriptor
on every call, so the cost grows with the total number of connections rather
than with the number of ready ones.

## Decision

Use `epoll` as the only readiness backend. Its cost is proportional to the
number of *ready* descriptors, not the number of *watched* ones, which is what
makes the C10K target reachable on a single core.

## Consequences

- Linux only. There is no portable fallback and none is planned before 1.0
  (`kqueue` is a long term idea).
- `epoll_wait` is the single deliberate blocking point of the event loop; every
  other syscall runs on non-blocking descriptors.
- The completion based `io_uring` API stays a post-1.0 idea for comparison.
