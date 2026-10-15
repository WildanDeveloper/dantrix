# Contributing to Dantrix

Thanks for considering it. This is a learning project: the code is meant to be
read, so clarity counts more than cleverness.

## Before you start

- **Read `SECURITY.md` first.** It states the threat model, and a change that
  weakens a guarantee there needs to say so explicitly.
- **This project is pre-alpha and does not support TLS.** Do not propose
  deploying it; do not add features that assume a trusted network.
- **Everything is in English**: code, comments, commit messages, documentation,
  issues and pull requests. The maintainer may converse in Indonesian, but the
  artifacts stay English so the project is readable to anyone.

## Getting set up

```bash
git clone https://github.com/WildanDeveloper/dantrix
cd dantrix
make            # release build, zero warnings required
make test       # unit + integration
```

Requirements: a C11 compiler (gcc or clang), GNU make, Linux headers. `clang`,
`cppcheck` and `python3` are needed for the full gate. There are no third-party
dependencies, and that is a design rule rather than a coincidence.

## The gate a change has to pass

Run these before opening a pull request. CI runs the same set.

```bash
make test              # unit tests, then the integration suites
make test-asan         # the same under ASan+UBSan
make test-tsan         # thread sanitizer (multi-worker paths)
make test-regression   # the defect regression suite
make test-security     # attacks a live server; every SECURITY.md claim
make test-toctou       # races the filesystem against the server
make test-seccomp      # the sandbox under a full workload
make test-config       # the documented configuration surface vs the binary
make lint              # cppcheck
make format-check      # clang-format
make fuzz              # the deterministic fuzz drivers (no Clang needed)
```

Longer runs, when the change touches memory or concurrency:

```bash
make soak              # 1 hour of mixed load, RSS/fd trend verdict
make fuzz-long HOURS=1 # a libFuzzer campaign
```

## What a good change looks like

**A test that can fail.** This is the rule the project cares about most. A test
that passes whether or not the code is correct is not evidence. Before trusting
a new check, break the code on purpose and confirm the check fails. Several
defects in this project were found only because a test was shown to catch an
injected bug first; two fuzz invariants and one soak threshold were wrong in ways
that made them unfalsifiable, and the fix came from measuring the real behaviour.

**A comment that explains why.** The code says what; the comment says why it is
that way, especially when the obvious alternative is wrong. Several comments in
this tree record a mistake that was made and corrected, because the difference
between the right and wrong version is a memory-ordering argument or a protocol
detail that is not visible from the code alone.

**Honesty about what was measured.** If you claim a performance or memory
number, say what it was measured on and with what tool. `bench/measure.py`
prints the environment and the results together for exactly this reason. A
number without its environment is not a result.

**No new dependencies.** The project builds with libc and the Linux kernel. A
change that needs a library needs a discussion first.

## Coding rules

These are enforced by `-Werror` and by `make lint`:

- C11, `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion
  -Wstrict-prototypes -Wmissing-prototypes -Wnull-dereference -Wundef`.
- No `strcpy`, `strcat`, `sprintf`, `gets`. Use the bounded helpers.
- Every syscall result is checked; `EINTR`, `EAGAIN` and `EWOULDBLOCK` are
  handled explicitly.
- All descriptors are `CLOEXEC`; sockets are also `NONBLOCK`.
- Client data is never a format string and never reaches a response header
  verbatim.
- No allocation in the hot path after warm-up.
- `clang-format` decides the layout. Run `make format` rather than arguing about
  it.

## Where to put things

| What | Where |
|---|---|
| A parser or protocol rule | `src/http_parser.c`, with a case in `tests/unit/test_http_parser.c` |
| A filesystem or path rule | `src/path.c`, `src/file.c`; a traversal case in `tests/security_test.py` |
| A response header | `src/http_response.c`; the shape is asserted in `tests/unit/test_http_response.c` |
| A configuration key | the registry in `src/config.c`; `--print-config` and the README table are generated from it, so nothing else needs updating |
| A runtime invariant | `tests/regression_test.py`, and break it once to prove the test catches it |
| A fuzz invariant | `fuzz/fuzz_*.c`, and verify it fails on a deliberately broken build |
| A security claim | add the row to `SECURITY.md` **with how it is verified**, and the check to `tests/security_test.py` |

## Reporting a bug

For a security issue, use a private advisory (see `SECURITY.md`), not a public
issue. For anything else, an issue with a minimal reproduction is ideal: the
exact request bytes, the configuration, and what you expected instead.

## Commits

- One logical change per commit. A fix and its test belong together.
- The subject line says what changed and why in one line; the body explains the
  reasoning when it is not obvious.
- If you fixed a defect, say what the defect was, how it was found, and how you
  proved the fix works. "Fixed X" without that is a claim, not a report.

## Review

Expect questions about *why*, not just *what*. A change that is correct but
unexplained will be asked to explain itself, because the next reader is the
point of this project.
