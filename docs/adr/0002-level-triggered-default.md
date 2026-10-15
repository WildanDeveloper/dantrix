# ADR 0002: level-triggered epoll is the default

- Status: accepted
- Date: 2026-10-11
- Deciders: project maintainer

## Context

`epoll` offers two notification modes:

- *level-triggered*: an event is reported as long as the descriptor stays
  ready. Partial reads are safe; missing a read is recovered on the next
  wakeup. More wakeups.
- *edge-triggered*: an event is reported only on state change. Fewer wakeups,
  but the application must drain every descriptor to `EAGAIN` and register
  `EPOLLOUT` carefully or events are lost.

## Decision

Level-triggered is the default mode for v0.1 and remains the default for the
whole 0.x series. Edge-triggered becomes a configuration option (`edge_triggered`)
only when it can be measured against the default with the v0.7 benchmark suite.

## Consequences

- Correctness first: no lost event class, simpler state machine, easier
  debugging.
- One rule must still hold: `EPOLLOUT` is registered only while output is
  pending. A socket is almost always writable, so registering `EPOLLOUT`
  permanently would busy loop even in level-triggered mode.
- Per-connection read budgets (a bounded number of read syscalls per event)
  prevent one busy connection from starving the others.

## Update (2026-10-15)

The condition in the decision -- "only when it can be measured against the
default with the v0.7 benchmark suite" -- was met when v0.7 shipped with
`bench/`, but the option was never implemented. `edge_triggered` is accepted by
the configuration, warned about at start-up and otherwise ignored; the server
always runs level-triggered. The decision above still stands on its own
(correctness first), and the open question is whether to delete the key or
implement it -- recorded in
[`docs/v1.0-readiness.md`](../v1.0-readiness.md). Until that is decided, the ADR
is **accepted but partially unfulfilled**, which is stated here rather than
implied.
