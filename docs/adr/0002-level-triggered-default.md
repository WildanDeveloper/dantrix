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

Level-triggered is the default mode for v0.1 and remains the only mode. The
option to select edge-triggered was dropped before v1.0 rather than left in the
configuration as a switch that does nothing.

## Consequences

- Correctness first: no lost event class, simpler state machine, easier
  debugging.
- One rule must still hold: `EPOLLOUT` is registered only while output is
  pending. A socket is almost always writable, so registering `EPOLLOUT`
  permanently would busy loop even in level-triggered mode.
- Per-connection read budgets (a bounded number of read syscalls per event)
  prevent one busy connection from starving the others.

## Update (2026-10-15)

The original decision said edge-triggered would become an option "only when it
can be measured against the default with the v0.7 benchmark suite". v0.7 shipped
with `bench/`, so the condition was met, but the option was never implemented:
`edge_triggered` was accepted by the configuration, warned about at start-up and
otherwise ignored. The decision above still stood on its own (correctness
first), which left two ways out: implement it, or delete the key.

## Resolution (2026-10-17, before the v1.0 freeze)

The key was **deleted**. Setting `edge_triggered` is now an unknown-key error
(exit code 2, verified), and the documentation no longer offers a mode that does
not exist. The reasoning:

- A configuration key that appears to configure something and does not is a trap
  for the operator who sets it and believes it took effect. That is the same
  defect class as `add_header` before v0.8.10.
- Implementing edge-triggered means auditing every read and write path for the
  "drain until `EAGAIN`" discipline, where a mistake is a hang, not a slowdown.
  That work was not done, so promising it in a frozen surface would have been a
  claim without the evidence.
- Removing a key is a breaking change, and this is the last release where it is
  free.

The ADR is now **fully realised**: one mode, implemented, with no dead switch.
