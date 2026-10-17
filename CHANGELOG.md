# Changelog

All notable changes to Dantrix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project versions with
[Semantic Versioning](https://semver.org/). While the version is `0.x`, the
interfaces (CLI, configuration keys, HTTP behaviour) may change between minor
releases.

## [Unreleased]

### Added

- (this section collects changes since the last release)

## [1.0.1] - 2026-10-17

Documentation only: the README now agrees with the release it describes.

### Fixed

- v1.0.0 shipped with the README telling two opposite stories. The status table
  said "Current release v1.0.0 (frozen)" while the roadmap said "Configuration
  keys and HTTP behavior frozen ... **Not ready yet**" and marked the stable
  release as still planned. The freeze box is closed now and states what is
  frozen (49 configuration keys, 46 CLI flags, the HTTP behaviour, the status
  JSON field names, and the `common`/`combined`/`json` access-log formats).
- The badge and three prose blocks still described the project as pre-alpha.
  A frozen 1.0 API is not pre-alpha, but it is also not audited, and those are
  different claims: the badge now says "stable API, not audited", the banner and
  the FAQ answer say the same thing, and the deployment warning says "no
  independent security audit" instead of "still pre-alpha".

### Added

- `tests/config_test.py` (42 checks, was 40): at a 1.x version, no document may
  describe the freeze as pending, and every box in the roadmap's v1.0
  definition-of-done must be closed. Both checks were proven able to fail by
  re-opening the freeze box and by restoring the "Not ready yet" sentence.

## [1.0.0] - 2026-10-17

The surface freeze. The maintainer chose v1.0 on 2026-10-17, and the three
blockers recorded in `docs/v1.0-readiness.md` were resolved in this release
rather than left open. From here, the 49 configuration keys, 46 CLI flags, the
HTTP behaviour, the status JSON and the access-log formats are a promise: a
breaking change to any of them needs a major version.

### Changed (breaking, and the last release where that is free)

- **`edge_triggered` was removed.** It had been accepted since v0.6, warned
  about at start-up, and never implemented: the server only ever ran
  level-triggered. A switch that appears to configure something and does not is
  a trap, which is the same defect class as `add_header` before v0.8.10.
  Setting the key is now an unknown-key error (exit code 2, verified), it no
  longer appears in `--print-config` or the README table, and the key count is
  49. ADR 0002 records the resolution: one mode, implemented, no dead switch.
- Multi-range requests are now documented as a **permanent** deviation: the
  server ignores `Range: bytes=0-10,20-30` and answers a full `200`. RFC 9110
  permits ignoring `Range`; the README now says the behaviour will not change
  without a major version, so a client author can rely on it.

### Added

- **`log_format = json` is implemented.** It used to be documented and rejected
  with exit code 2. The access log now emits one JSON object per line with the
  field names `ts`, `peer`, `method`, `target`, `protocol`, `status`, `bytes`,
  `referer` and `user_agent`. The JSON escaper writes `\"`, `\\`, `\b`, `\f`,
  `\n`, `\r`, `\t` and `\u00XX`; the common format's `\xHH` escape is not valid
  JSON and would make a pipeline reject the line. Verified by parsing every
  emitted line with `json.loads`, including a target containing a quote and a
  backslash and a `User-Agent` containing a quote.

### Fixed

- The JSON branch in `dx_log_access` first appended the whole common-format line
  and then the JSON object, so every `json` line carried both. Found by running
  the format instead of reading it; the branch now returns before the
  common/combined fields are appended, and the test parses the lines.

### Added (tests)

- `tests/config_test.py` (40 checks, was 38): the removed `edge_triggered` key
  must be rejected with exit code 2 and named in the message; `log_format = json`
  must be accepted; every emitted JSON line must parse; the object must have
  exactly the documented field names; a quoted/backslash target must survive
  intact; a 404 must be recorded. The JSON checks were proven able to fail by
  reinstating the old `\xHH` escaper, which produced
  `Invalid \escape` from the JSON parser.

## [0.8.21] - 2026-10-17

One commit after v0.8.20: the comment added to the stage-2 example did not
compile.

### Fixed

- `examples/02_echo_thread_per_conn.c`: the comment explaining how to find the
  PID contained a `sed` expression, `s/.*:9001 .*pid=\([0-9]*\).*/\1/p`. The
  `*/` inside that expression closed the block comment early, so the rest of
  the line became code: both gcc and clang reported a stray backslash, an
  unterminated character constant and an unknown type name from the header
  chain. Every CI build job failed on v0.8.20. The comment now uses `awk` and
  contains no `*/`; all four examples build with gcc and clang under `-Werror`,
  and the documented one-liner was run against a live server.

### Note

v0.8.20's tag points at the commit whose build jobs failed. v0.8.21 is the
first green release in this series and carries the same changes plus this fix.
The local gate list did not include `make examples`; that is why a change to an
example could reach CI unbuilt.

## [0.8.20] - 2026-10-17

The first audit of `examples/` and `bench/`: every script was run instead of
only compiled, which found three defects in the learning material and one
promise the benchmark harness never kept.

### Fixed

- The examples table told the reader to test stage 4 with
  `curl -i http://localhost:9000/`, but `04_hello_http.c` listens on **9003**
  (9000 belongs to stage 1). Verified by running it: port 9000 does not answer,
  9003 returns `200 OK`.
- The compile snippet named `build/echo_epoll` and `build/echo_threads`, which
  no command produces; `make examples` builds `build/03_echo_epoll` and
  `build/02_echo_thread_per_conn`, after the source names. The snippet now lists
  all four examples with the names that actually exist, and says which port each
  one uses.
- The advanced-stage table in the learning section had two rows fused into one
  (`| v0.5 || Operations, ...`), so the `v0.6 to v0.8` row was not a table row.
- The stage-2 comment documented `pidof 02_echo_thread_per_conn` to count
  threads. Linux truncates the process name to 15 characters
  (`02_echo_thread_`), so `pidof` and `pgrep` by name both find nothing — the
  command that exists to prove the model does not scale printed an empty PID.
  The comment now takes the PID from the listening socket, verified to work.

### Added

- `bench/measure.py --save` writes the record the README has always promised:
  `bench/results/<date>-<commit>/` with `results.md`, `environment.txt`
  (hardware, kernel, compiler, binary, commit, server flags) and `command.txt`.
  Before this, no script wrote to `bench/results/` at all;
  `compare_reference.sh` only told the reader to record the output by hand.
  Verified by running it: the three files are written with the values measured.
- `bench/results/README.md` described the format as a single `.txt` file, which
  contradicted the README's directory format, and the file was not tracked
  because `/bench/results/` was ignored wholesale. It now describes the
  directory, and `.gitignore` keeps the directory ignored while tracking the
  README (`/bench/results/*` plus a negation), so a fresh clone has it.
- `tests/config_test.py` (38 checks, was 33): the examples table's port must
  match the source's `#define PORT`; every example must use a distinct port;
  the compile snippet's `-o build/<name>` must match the source's basename; the
  benchmark harness must be able to save the record and `bench/results/README.md`
  must describe it.

### Verified while auditing

- Stage 1 blocks a second client while the first is connected (measured: second
  client got no echo for 1 s).
- Stage 2 creates one thread per connection (measured: 8 connections = 9
  threads, back to 1 when they close). With 10,000 idle connections it needed
  42 s to accept them and reached 6,557 threads / 78 MiB RSS, against 14.9 MiB
  for Dantrix at the same count — the "does not scale" claim is measured, not
  asserted.
- Stage 3 serves 20 concurrent clients with no `EPOLLET` anywhere in the
  sources, matching the "level-triggered" label.
- `bench/py_loadgen.py` (6,364 req/s at 32 connections) and
  `bench/slowloris.py` (200 slow connections, server stayed responsive and
  answered `200 OK` afterwards) both work as documented.
- `bench/run_wrk.sh` ran every scenario it documents (S1 28,856; S2 10,348;
  S3 5.79 req/s at 1.22 GB/s; S4 13,436; S7 12,361 req/s) and skipped S6 with a
  message when `wrk2` is absent. `bench/compare_reference.sh` compared Dantrix
  against `python3 -m http.server` on the same payloads and, as documented, did
  not start or stop either server.

## [0.8.19] - 2026-10-16

The deployment artifacts and the copies of them embedded in the README had
drifted apart, and the installed systemd unit never read the installed
configuration.

### Fixed

- The systemd unit started the server with environment variables and without
  `--config`, so the configuration that the install steps place at
  `/etc/dantrix/dantrix.conf` was never read and `systemctl reload` could not
  pick up an edit. `deploy/dantrix.service` now runs
  `ExecStart=/usr/local/bin/dantrix --config /etc/dantrix/dantrix.conf` and
  grants `CAP_NET_BIND_SERVICE`, so ports below 1024 work without root. The
  grant was verified with `setpriv`: the same binary binds port 80 as `nobody`
  only when the capability is ambient, and fails with `Permission denied`
  without it.
- The README's quick start ran the container with
  `-v "$PWD/public:/var/www:ro"` while the image serves `/var/www/dantrix`
  (`ENV DANTRIX_ROOT`): following it produced a server serving an empty
  directory (404 for everything). Both `docker run` examples now mount
  `"$PWD/public:/var/www/dantrix:ro"`.
- All six files the README embeds differed from the files on disk. The embedded
  Dockerfile still said `/var/www`, ran `make release` without `make clean` and
  carried Indonesian comments; the embedded logrotate file was missing
  `su dantrix dantrix` and `create 0640 dantrix dantrix`; the embedded systemd
  unit was a different design from the installed one; the embedded example
  configuration showed the compiled defaults while the heading claimed it was
  `conf/dantrix.conf`, which deliberately differs from them; the compose copy
  mounted `/var/www` and the sysctl copy lacked the comment header. The README
  now shows every file byte-for-byte.
- `conf/dantrix.conf` said it "deliberately differs from the compiled defaults
  in a few places" without naming them, and its `edge_triggered` comment still
  pointed at v0.7, which shipped without implementing the key. The header now
  names the three differences and the key's comment matches the start-up
  warning (no effect, warns at start-up).

### Added

- `conf/dantrix.conf` now also shows the optional `mime_types`, `cache_control`,
  `add_header`, `socket_rcvbuf` and `socket_sndbuf` keys as comments, so the
  file is the full example the README always described.
- `tests/config_test.py`: the README must embed the example configuration and
  every file in `deploy/` byte-for-byte (the list comes from the directory, so
  a new artifact must be documented); every container example must mount the
  document root where `DANTRIX_ROOT` points, read from the Dockerfile, and must
  not mount an ancestor of it; the example configuration must validate from the
  repository root and differ from the compiled defaults exactly where its
  header says.

## [0.8.18] - 2026-10-15

The version-drift check: the class of defect that produced every fix in the last
three releases, now caught by a test instead of by reading.

### Fixed

- The README's status table said **"Current release: v0.8.14"** while the binary
  was at 0.8.17. This is the same drift the directory tree and the key counts had:
  a number written once and never revisited. It is now v0.8.17, and a test keeps
  it that way.
- `docs/v1.0-readiness.md` had no statement of which release it describes, so a
  reader could not tell whether its counts were history or a current claim. It now
  carries a review marker (`Written at v0.8.11 and reviewed at v0.8.17`), and the
  test requires that marker to name the release the binary reports.

### Added

- Two checks in `tests/config_test.py`:
  - every "current release" mention in `README.md` and
    `docs/v1.0-readiness.md` matches `dantrix --version`;
  - the readiness document's review marker matches it too.
- The suite now runs 22 checks, and every one of them has been shown able to fail
  by injecting the defect it exists to catch: the stale version in the README,
  the stale review marker, the "(45 keys)" claim, the "planned for v0.7" message,
  and a nonexistent path in the directory tree.

## [0.8.16] - 2026-10-15

The documentation audit that the new configuration test made possible. Two more
classes of drift, both invisible to every test that starts a server.

### Fixed

- **The README's directory tree described files that do not exist.** It was
  written as a target ("some files are only created as the versions progress")
  and never reconciled, so it claimed `CODE_OF_CONDUCT.md`, `.clang-tidy`,
  `.github/workflows/sanitizers.yml`, `.github/workflows/release.yml`, four ADR
  files and a `docs/` set (`ARCHITECTURE.md`, `PROTOCOL.md`, `TUNING.md`,
  `BENCHMARKING.md`, `SECURITY_MODEL.md`) that were never created. It also
  missed 11 real source files, five real test files and the two ADRs that do
  exist. It is now generated from the working tree and says so.
- **Three Indonesian fragments in English artifacts**, against the project's own
  rule: a comment in the README's code sample (`indeks + generasi, bukan
  pointer mentah`), the tree's `(GCC dan Clang)`, and the ADR filename
  `0001-epoll-bukan-select-poll.md` -- which was also **wrong**, since the file
  on disk is `0001-epoll-not-select-poll.md`.
- ADR 0002 is now labelled **accepted but partially unfulfilled**: its decision
  said edge-triggered would become an option "only when it can be measured
  against the default with the v0.7 benchmark suite", v0.7 shipped with `bench/`,
  and the option was never implemented. The original text is untouched; an
  update section records what happened and points at the readiness document.

### Added

- More checks in `tests/config_test.py`, all shown able to fail:
  - **every path in the README's directory tree exists on disk** (injecting
    `.clang-tidy` into the tree fails the test);
  - the three **runtime-generated directories** (`fuzz/corpus/`,
    `fuzz/artifacts/`, `bench/results/`) are marked as generated in the tree,
    because they are git-ignored and legitimately absent from a fresh clone;
  - every CLI flag has help text, and the flag count is counted rather than
    copied from prose.
- The suite now runs 18 checks.

### Notes

- **CI caught a real weakness in the new check, which is the useful part.** The
  tree check passed locally and failed on the runner with
  `claimed but absent: ['artifacts', 'corpus', 'results']` -- the three
  git-ignored runtime directories. The failure proved the check reads the clone
  rather than the author's working tree, so it was fixed by marking those three
  entries as generated and exempting them, not by weakening the check. It was
  then re-verified against a tree with those directories deleted.

## [0.8.15] - 2026-10-15

A new test suite for the one surface nothing tested: the configuration itself.
It found three stale claims on its first run.

### Added

- **`tests/config_test.py`** (`make test-config`), which reads the configuration
  surface the way an operator does: it writes configuration files, starts the
  binary and checks what the process does. It enforces four rules -- a documented
  key must be registered; a key that is accepted but has no effect must say so on
  stderr; a value that cannot be used must be rejected loudly rather than silently
  downgraded; and a rejected configuration must exit 2 without leaving a log or
  pid file behind. It also cross-checks the README's stated key count against the
  rows in its table and confirms every CLI flag has help text.
- The suite runs in CI on every push, against the ASan build, alongside the other
  standalone suites.

### Fixed

- **Three stale claims, all found by the new suite:**
  - `src/main.c` told the operator `edge_triggered is not implemented yet
    (planned for v0.7)` -- a version that shipped nine releases earlier. The
    message now states the key has no effect and names no release, because a
    promise with a version in it goes stale.
  - The README still listed `add_header` as "announced as not implemented" when
    it has worked since v0.8.10 (verified: the header arrives). Only
    `edge_triggered` is a no-op now, and the README says so.
  - The README's reference table was announced as "(45 keys)" while it contains
    50 rows, and `docs/v1.0-readiness.md` claimed 49 keys and 45 CLI flags. The
    verified counts are **50 configuration keys** and **46 CLI flags**; the
    readiness document now carries the method, not just the number.
- The README's directory listing was missing five test files: `config_test.py`,
  `security_test.py`, `seccomp_test.py`, `toctou_test.py` and `soak_test.py`.

### Notes

- The new suite was shown able to fail before it was trusted: restoring the
  "planned for v0.7" message makes it fail, and restoring the "(45 keys)" claim
  makes it fail. It also **fails against the v0.8.14 binary**, which is the
  regression check that proves it tests the binary rather than itself.
- This is the class of defect the rest of the suite could not see. Every other
  test starts a server and makes requests; none of them read the documentation
  against the binary, so prose could drift for releases without anything
  contradicting it.

## [0.8.14] - 2026-10-15

The C10K criterion, both halves of it.

### Added

- **`tests/c10k_test.py` now runs the roadmap's "plus 1,000 active" half**, not
  only the idle half. Holding idle connections is the easy part: an idle
  connection owns no read buffer. The claim worth testing is that 1,000 clients
  can be served *while* thousands sit idle, because then the server is working
  with a large descriptor table, timer wheel and connection pool at the same
  time.
- It samples RSS and the descriptor count before and after the active burst, so
  the cost is attributed to the active clients rather than to the total.

### Verified

- **C10K passes at the roadmap's size**, 10,000 idle plus 1,000 active:

  | Measurement | Result |
  |---|---|
  | 10,000 connections opened | 0.9 s, all accepted, all answering |
  | 1,000 active clients, 10,000 held idle | 1,000/1,000 served in 0.1 s |
  | RSS during the burst | 51.8 -> 52.8 MiB (**+1.05 MiB**, ~1.1 KiB per active client) |
  | Descriptors | 10,008 -> 11,008 (**+1,000**, exactly the new clients) |
  | The idle set | survived the burst |
  | Shutdown with all 11,000 open | exit 0, clean |

  The growth being proportional to the *active* clients is the point: a server
  that allocated per open socket would have grown ten times as much.

- CI runs the same test at 2,000 idle plus 1,000 active on every push, so the
  active half is exercised continuously rather than once.

## [0.8.13] - 2026-10-15

The 1-hour soak, complete: the last long run of the v0.8 milestone.

### Verified

- **`make soak` passed at the roadmap's one-hour length.** 3,601 seconds,
  700,671 requests (195/s against a 200/s target), 720 samples:

  | Metric | Result | Budget |
  |---|---|---|
  | RSS | 2.4 -> 8.2 MiB, **fitted slope +1.64 MiB/h** | 4.00 MiB/h |
  | Descriptors | 217 -> 217, **slope 0.0/h** | 10/h |
  | `fds_actual - fds_believed` | 5 -> 5, constant | must not move |
  | Still serving afterwards | yes | -- |

  The RSS growth is a **one-off**: +5.77 MiB as the pools reach steady state,
  then flat. That is the distinction the trend verdict exists to make -- a leak
  would grow in proportion to 700,671 requests, and the fitted slope would be
  hundreds of MiB per hour rather than 1.64.

- Zero descriptor growth over 700,671 requests confirms the v0.7.3 leak fix
  holds under sustained load, not just in the short regression test.

- The test's ability to fail was established before its pass was trusted:
  injecting 4 KiB of leaked heap per request turned the same run into +6.68 MiB
  in 45 seconds and failed it.

### Notes

- The first attempt at this run was **invalid and was discarded**: a `pkill -x
  dantrix` inside an unrelated command killed the server being measured two
  minutes in. That is the exact failure mode the README warns about for this
  test, so the run was thrown away and restarted rather than reported. The
  process check is now part of the command that does anything else.

## [0.8.12] - 2026-10-15

The 24-hour fuzz campaign, complete, with what it does and does not prove.

### Verified

- **The 24-hour campaign finished on its own timer on all three targets:**

  | Target | Runs in 24 h | Branches reached |
  |---|---|---|
  | parser | 1,148,809,658 | 239 |
  | path | 657,222,568 | 313 |
  | escape | 368,894,894 | 66 |
  | **total** | **2,174,927,120** | |

  Zero crashes and zero artifacts: `fuzz/artifacts/` contains the three empty
  directories the campaigns were pointed at. Both runs ended with
  `Done N runs in 86401 second(s)`, so neither was truncated.

### Notes

- **What the number does not mean.** The campaign saturated: coverage stopped
  growing well before the end, with `cov 313` appearing 16 times in the last 2,856
  milestones where earlier values appeared hundreds of times. The billions of
  executions are mostly repeats of already-known paths, so "2.17 billion runs" is
  not "2.17 billion distinct scenarios". The honest summary is: no random input
  found a way to crash, hang or corrupt the server, and the search space was
  exhausted early.
- **What actually found the bugs.** Every defect fixed during v0.8 came from
  somewhere other than this campaign: the descriptor leak from the accounting
  audit, the per-IP slot race from TSan, the slowloris hole from a test written to
  falsify the documentation, the `add_header` CRLF injection and the
  valid-config-breaks-everything bug from implementing and reviewing the feature.
  Fuzzing covers input handling; it cannot see concurrency, time, or a
  configuration that is accepted and then ignored.
- The harness was proven able to fail before the campaign was trusted: injecting
  a wrong-entity bug made the escape fuzzer report it, after the original
  invariant had missed the same bug across 300,000 iterations.

## [0.8.11] - 2026-10-14

Status reporting while the long runs finish, and a v1.0 readiness analysis.

### Added

- **`docs/v1.0-readiness.md`**: an analysis of whether the public surface is ready
  to freeze for v1.0, written before any decision rather than after. It counts the
  surface (49 configuration keys, 45 CLI flags, 18 documented status codes, the
  status JSON), lists the four things that still block a freeze, and separates
  what has been stable for many releases from what is still moving.
  The blockers are concrete and were each verified against the running binary:
  `edge_triggered` is accepted and ignored with a warning that names v0.7 (nine
  releases ago); `log_format = json` is documented and rejected at parse time;
  multi-range requests are a deliberate, permanent-looking deviation from RFC
  9110; and the `header_timeout` semantic only became a deadline seven releases
  ago and has not run outside this machine.
  The recommendation is **not to freeze yet**: a v1.0.0 tag now would promise a
  dead key, an unusable documented value and an unexercised timeout, and fixing
  any of them afterwards would need a major version.
- **A live status section in the README** ("Where the project is right now"),
  because a reader could previously only infer the state from a table of
  checkmarks. It states the current release, what is complete, what is running,
  and what is blocked on what -- and it says the checkpoint goes stale, so the
  numbers carry the date they were taken.

### Changed

- The 24-hour fuzz campaign is reported as **in progress with its measured
  numbers** (469 million executions, zero crashes, coverage still growing) rather
  than as a ticked box. The roadmap item stays open until the runs finish; a
  claim is not marked done because it is close to done.
- The v1.0 definition of done now links the readiness analysis, and the
  "frozen according to Semantic Versioning" item says plainly that it is not
  ready and points at the specific reasons.

## [0.8.10] - 2026-10-14

`add_header` implemented (it was accepted and ignored), and CONTRIBUTING.md.

### Added

- **`add_header` now works.** It was accepted by the configuration, printed by
  `--print-config` and then **silently ignored** -- the only key in the registry
  with no effect, announced by a start-up warning that most operators would never
  read. It is emitted on every response now, on all eight response paths
  (200/206/301/304/400/404/405/416/501 and the JSON status endpoint), verified by
  a test that counts occurrences per path.
- **`CONTRIBUTING.md`**: setup, the gate a change must pass, and the rules this
  project actually cares about -- a test that can fail, a comment that explains
  why, and honesty about what was measured.

### Fixed

- **A configured `add_header` could inject a response header.** The value was
  written into the response verbatim and never validated, so a CR or LF in the
  configuration split the response -- the same attack the request parser refuses
  on the input side, reachable through a config file instead of a request.
  `dx_header_value_valid()` now rejects CR, LF, NUL, a missing colon, an empty
  name and whitespace in the name, and the shape must be `Name: value`.
- **A valid configuration could make every response fail.** The staging buffer
  is 2 KiB; fourteen headers of 250 bytes are 3,500 bytes, and the configuration
  accepted that and then answered 500 to everything. Measured: the server
  returned an empty response for a config it had just accepted. The total is now
  checked at start-up against the buffer minus a 512-byte reserve for the status
  line and the fixed headers (measured at 178 bytes for a full response), and the
  error names both numbers.
- **Framing headers are refused rather than warned about.** `Content-Length` and
  `Connection` in the configuration would let a config file create a
  request-smuggling primitive in front of a proxy, so they are a start-up error.
  Headers the server already sends for its own reasons (`Server`, `Date`,
  `X-Content-Type-Options`) are a warning: sending them twice is legal, and an
  operator who copies a standard security-header set into the config should not
  have the server refuse to start.
- The first implementation emitted the configured headers **twice** on error
  responses, because they were added in two places at once. They are added in
  `response_assemble()` alone, which every response path goes through, so a path
  added later cannot forget them and cannot double them either.

### Tests

- `test_add_header` (29 checks): validation, emission exactly once on success and
  error responses, coexistence with the caller's own headers, and a configuration
  that just fits the buffer still rendering completely.
- A security test writes a real CRLF into a config file and asserts the server
  refuses to start, and asserts a realistic security-header set reaches both a
  200 and a 404 exactly once each.

### Notes

- Three assertions in the new tests were wrong before they were right: a trailing
  newline expectation on a response that ends with a body, a "worst case fits"
  check that contradicted the limit the validation enforces, and a CRLF test file
  that contained the two characters `\r\n` literally instead of the bytes. Each
  was fixed by measuring the real behaviour.

## [0.8.9] - 2026-10-14

Re-measured with `wrk`: the syscall work finally has a number.

### Added

- **`bench/measure.py`**: runs the README scenarios, prints the environment table
  and the results together, and refuses to report a number without the context
  it came from. It uses `wrk` and warns loudly when only the Python client is
  available, because that client is its own bottleneck.

### Changed

- **The benchmark tables are current again, measured at v0.8.8 with `wrk`.** The
  previous numbers came from v0.7.0 and the Python client, and were marked as
  historical. `wrk` is now available on this machine, so the latency columns
  exist for the first time.
  - S1 (1 KiB, 256 connections): **22,493 req/s** with the open-file cache off,
    **30,514 req/s** with it on, p99 28.46 ms -> 22.08 ms. Same binary, same run,
    one option apart: the cache is worth **+36% throughput and 22% lower p99**,
    which is the first measured number for the syscall reduction shipped in
    v0.7.3.
  - S3 (1 MiB): 1,346 req/s, 1.31 GB/s.
  - S4 (1 KiB): 30,379 req/s at 1,024 connections, 16,672 at 4,096. The drop is
    the load generator and the server sharing two cores, and the README says so
    instead of presenting it as a server limit.
  - S5 (idle): 10,000 connections cost 14.9 MiB of RSS, **1.32 KiB per
    connection**.

### Documentation

- The per-idle-connection design target is **no longer claimed as met.** It says
  ≤ 1 KiB, and the measurement is 1.32 KiB. The earlier 1.2 KiB figure predated
  the v0.8 descriptor accounting. Reporting the measurement and stating that the
  target is missed is the only honest option; rounding 1.32 down to "about 1"
  would have made the table agree with the target and disagree with reality.
- Removed the last `TBD` placeholders from the README. A cell now either holds a
  measurement with its environment or says the scenario was not measured here.

### Notes

- Measuring required stopping the three 24-hour fuzz campaigns first: load
  average was 4.65 on a 2 vCPU machine, and any number taken then would have
  described CPU contention rather than the server. The campaigns had reached 269
  million executions between them with zero crashes and were resumed after.

## [0.8.8] - 2026-10-14

The soak verdict no longer fails on a sanitizer build, and the reason is measured.

### Fixed

- **The soak test failed in CI on the sanitizer build with +12.90 MiB of RSS
  growth, and that was a false positive.** CI runs the soak against the ASan
  binary, and AddressSanitizer grows without bound by design: it keeps metadata
  per allocation (shadow memory, allocation stacks) and a quarantine of freed
  memory, so RSS climbs as new allocations appear. The growth tracks allocation
  *uniqueness*, not a leak in the program.
  Measured on this machine, same workload, same source:
  - **release build**: 2,176 kB -> 8,284 kB and then **exactly flat for 32
    seconds** (the one-off is the pools reaching steady state);
  - **ASan build**: 21.8 MiB -> 33.4 MiB and still climbing at the end.
  A threshold that fits the release build fails every sanitizer run; a threshold
  that fits the sanitizer would miss a real leak. The soak therefore judges the
  **release** build and reports a sanitizer build's trend without a verdict,
  saying so in the output.
- The sanitizer detection inspects the whole path, not just the file name:
  `build/asan/dantrix` and `build/dantrix-asan` are both recognised, which the
  first version missed for one of the two shapes.

### Verified

- The verdict still fails on a real leak in the release build: injecting 4 KiB of
  leaked heap per request produced +6.68 MiB in 45 seconds and failed the run.
- The release build passes the same run with +0.03 MiB.
- This is the second time the soak test itself was the thing that was wrong, and
  both times the fix came from measuring the server rather than adjusting the
  threshold until it passed.

## [0.8.7] - 2026-10-14

The soak test the roadmap claimed existed.

### Fixed

- **`make stress` did not stress anything.** It ran `bench/make_files.sh` and
  then *printed instructions* for a human to hold load and watch RSS by hand. No
  load was generated, nothing was measured and nothing could fail -- so the
  roadmap claim "a stable 1-hour stress test (RSS and fd count flat)" was never
  verified by anything. This is the same pattern the rest of v0.8 kept turning
  up: a claim that cannot fail is not evidence. The target now runs the soak
  test.

### Added

- **`tests/soak_test.py`** (`make stress`, `make soak`): holds idle connections
  open while active clients stream requests, samples RSS, the descriptor count
  and the believed/actual accounting gap on a fixed interval, and judges the
  **trend** rather than the difference between the first and last sample. A slow
  leak is invisible in a start-versus-end comparison, because allocator churn
  over a short run is larger than the leak, but it is unmistakable as a slope.
- The verdict distinguishes a leak from measurement noise in two ways, both
  learned by watching the test misjudge a flat server: a slope needs a minimum
  run length before it means anything (a 60-second fit over a few MiB of noise
  read as hundreds of MiB/hour while the process sat at exactly 2176 kB for a
  full minute), and a short run is judged on absolute change instead, with the
  substitution stated in the output rather than silently applied.
- The soak run pins `keepalive_timeout` past its own length. Without it the
  server closes the idle connections mid-run -- correct behaviour, but it made
  the descriptor count fall and would have looked like a finding.

### Verified

- The soak test catches a real leak: injecting 4 KiB of leaked heap per request
  turned a 2.1 MiB server into 11.5 MiB in 61 seconds and failed the run
  (`+9.34 MiB, 2605 requests`). With the leak removed the same run passes.
- A flat server is not flagged: measured 2,176 kB of RSS unchanged across a full
  minute of sustained requests, with the descriptor count constant at 13.

## [0.8.6] - 2026-10-14

Closing v0.8: the atomic document-root claim, falsified rather than assumed.

### Added

- **`tests/toctou_test.py`** (`make test-toctou`): races the filesystem against
  the server to try to falsify the claim that the document root is enforced
  atomically. A writer thread swaps the requested target between a legitimate
  file and a symlink pointing outside the root as fast as it can while four
  readers request it. If there were a check-then-open window, some request would
  land in it. Measured: **4,188 requests against 41,786 swaps, no leak.** The
  claim in SECURITY.md moves from "verified by construction, not falsified" to
  verified by attempted falsification.
- Security tests for the symlink policy: an absolute link out, a relative link
  out and a link that stays inside, under both `follow_symlinks` settings. The
  inside link is *served* when the option is on, so a blanket refusal cannot
  pass the test -- the policy has to be right, not merely strict.
- A test that pins the hard-link behaviour and asserts the documentation covers
  it, so the note cannot drift away from what the code does.

### Documentation

- **SECURITY.md now states that a hard link inside the document root is
  served**, and why that is not a bypass: a hard link *is* an entry of the root,
  indistinguishable from a regular file to the kernel, and every server serves
  it. The boundary this project guarantees is "nothing outside the root is
  reachable through a path", which a hard link does not violate. Previously this
  behaviour was undocumented, which is the one thing it should not have been.
- **`chroot` is documented as deliberately not implemented** rather than left as
  a roadmap gap: `RESOLVE_BENEATH` is a kernel guarantee attached to the open
  call, so it cannot be defeated by a path that changes between the check and the
  use, and it needs no privileged setup step. Adding `chroot` would be a second,
  weaker boundary on top of one that already holds.

### Notes

- The symlink policy was verified to be *differentiating*, not merely strict: with
  `follow_symlinks = false` even a link that stays inside is refused, and with it
  true that same link is served while the escaping ones are still refused. A
  blanket "deny everything" implementation would have passed a naive test.

## [0.8.5] - 2026-10-14

The 24-hour fuzz campaign, and the harness weakness it exposed.

### Added

- **A third fuzz target, `fuzz/fuzz_escape.c`**, for the two functions the
  security claims rest on: `dx_html_escape` (a file name becomes HTML in a
  listing) and `dx_url_encode` (a request target becomes a `Location` header).
  Neither was covered before, which is exactly the gap worth closing: they are
  pure functions over attacker-influenced bytes.
- **A seeded corpus** (`fuzz/seed_corpus.py`): 36 parser, 31 path and 20 escape
  inputs, including the traversal, smuggling and injection payloads the security
  tests use. A seeded corpus matters because libFuzzer reaches interesting code
  far sooner from a meaningful starting point than from random bytes, and every
  seed doubles as a regression input.
- **`make fuzz-long`** runs a libFuzzer campaign (30 minutes per target by
  default, `HOURS=24` for the full run) with a crash writing its reproducer to
  `fuzz/artifacts/`. **`make fuzz-replay`** re-runs every saved corpus and
  artifact input, so a finding stays a regression test.
- `dx_url_encode` is now public API (`src/file.h`) rather than a file-static
  helper, so it can be fuzzed and tested directly.
- `test_escape` (49 checks) pins the exact output for the five HTML entities, the
  percent-encoding rules, and the refusal-not-truncation behaviour on a small
  buffer.

### Fixed

- **The escape fuzzer could not detect a wrong entity, only a missing one.** Its
  invariant was "no raw `<`, `>`, `"` or `'` in the output", which is satisfied
  by mapping `<` to `&gt;` -- the output is quote-free and renders the wrong
  character. Demonstrated: with that bug injected, the driver ran 300,000
  iterations clean. The invariant is now a round-trip: decode the output and
  compare it with the input, which catches a wrong entity. Verified in both
  directions -- the correct code passes, and the wrong-entity injection now
  fails on the first seed.
- Two defects in the new round-trip helper itself, both caught by running it
  rather than by reading it: a capacity check that only guarded one branch, so a
  large input wrote past the buffer; and an expectation that ignored
  `dx_html_escape` dropping control bytes by design, which made the check fail on
  correct code. The comparison now strips controls from the expectation, matching
  the documented contract.

### Fixed (harness and CI coverage)

- **`fuzz_path` was never run by CI.** The sanitizer job built and ran the parser
  and (since this release) escape harnesses, but the path harness -- which covers
  path resolution, ranges and MIME, including the traversal payloads -- was only
  reachable by hand. It now has its own CI step under ASan+UBSan, so all three
  targets run on every push.
- `test_escape` gained a fixed-input round-trip check, so the property the fuzzer
  relies on is also enforced by `make test` and does not depend on a campaign
  having been run.

### Notes

- The campaign result is only meaningful because the harness was shown to catch
  a real bug first. "No crashes in 24 hours" from a harness that cannot fail is
  not evidence, and this release is as much about making the harness falsifiable
  as about the hours it ran.
- Three assertions in the new tests were wrong before they were right (a
  non-empty output expectation for an all-control-byte input, and twice
  comparing the escaped length with the decoded length). Each was fixed by
  measuring the actual behaviour rather than adjusting the code to match the
  test.

## [0.8.4] - 2026-10-14

A concurrency defect in the per-IP limit, found by the CI sanitizer job.

### Fixed

- **`max_conns_per_ip` could be exceeded under a multi-worker race.** Claiming an
  empty slot in the tracking table wrote the key first and only then checked
  whether the slot was still free. The thread that lost the race had already
  overwritten the winner's key bytes, so two client addresses ended up sharing
  one counter: one address's allowance silently doubled, which is the exact
  failure the limit exists to prevent.
  The claim is now a compare-and-swap on a dedicated state word, and the key is
  written **only by the winner, after it has won**. Proven with a probe that
  starts 16 threads on a barrier against a table sized to the number of keys and
  compares each key's own grants with its own counter: with the old code the
  counter halves for one key (grants 2000, counter 1000); with the fix it is
  exact over repeated runs.
- Found because the CI sanitizer job reported `per-ip: accepted 5` for a limit
  of 4 -- once, under load, in a job that had just started running the security
  tests. The race is narrow, so it took a purpose-built probe with a start
  barrier to reproduce locally rather than a plain traffic test.
- `dx_connlimit_count()` no longer casts away `const` to reuse the lookup: the
  cast would have allowed a future edit to write through a const handle, and it
  hid the fact that counting never claims a slot.
- `dx_connlimit_distinct()` now reports claimed slots rather than non-zero
  counters, so an address whose connections all closed is still counted as
  tracked until its slot is reused.
- **A second ordering bug, found while fixing the first.** Marking a slot
  claimed *before* writing its key let a lookup observe a claimed slot with an
  incomplete key: the address was never found, so its connections were counted
  nowhere and its limit was lost. The slot now has an explicit publication flag
  written last with release ordering, and a lookup only compares a slot once
  that flag is set. The claim order is now: reserve with a CAS (so exactly one
  worker owns the slot and a loser cannot clobber the key), fill in the key,
  then publish. Both wrong orders are documented in the code, because each looks
  reasonable on its own and the difference is a memory-ordering argument rather
  than something visible in the control flow.
  ThreadSanitizer caught this one directly, naming the exact lines
  (`slot_matches` reading `key_len` and `memcmp`-ing the key against
  `claim_slot` writing them), which is what turned a suspicion into a
  reproduction.

### Tests

- `test_connlimit` gained `concurrent_slot_claim` (137 checks total): 16 threads,
  16 distinct keys, a start barrier, three rounds, and a per-key comparison of
  grants against the counter. The earlier concurrency test compared a single
  total, which could not distinguish "two keys, two allowances" from "one key's
  allowance doubled" -- the assertion was wrong, not the code.

## [0.8.3] - 2026-10-13

Hardening step 4: a security review with the claims actually tested, and a
slowloris hole it found.

### Fixed

- **A client that dripped one byte at a time could hold a connection open
  forever.** The header timeout was refreshed on every read instead of being a
  deadline from the first byte, so a trickle reset it indefinitely -- the
  classic slowloris, and the exact opposite of what the README documented
  ("since `accept` or since the first byte of the next request"). The window is
  now stamped on the first byte of a request and never pushed forward; a
  complete request that arrives slowly inside the window still succeeds, which
  is the difference between bounding the phase and bounding the gaps.
  Proven: with `header_timeout=1s`, a 0.4s drip held the connection for 12.3s
  on the previous release and is closed after 1.6s now. The new security test
  fails against v0.8.2.
- The body timeout keeps its gap semantics on purpose: resetting on progress is
  what lets a slow but genuine upload finish. The two timeouts now behave
  differently because they protect different things, and both are documented.

### Added

- **`SECURITY.md`**: threat model, reporting process, known limitations, and a
  table of every security claim with *how it is verified*. A claim that cannot
  be tested is listed as unverified rather than assumed true.
- **`make test-security`** (`tests/security_test.py`): 34 checks that attack a
  live server the way an untrusted client would -- 12 path-traversal payloads
  against a secret file outside the root, a real symlink pointing outside,
  hidden-file probes, six framing/smuggling payloads, CRLF injection through
  the redirect path, open-redirect targets, six malformed `Content-Length`
  forms, 500 overlapping ranges, oversized request line/headers/body, log
  injection, the slowloris bound, and the build hardening of the binary itself
  (`readelf`: PIE, non-executable stack, RELRO, `BIND_NOW`; `nm`: fortified
  calls and the stack protector).

### Notes

- The review found no defect in the path, parser, header-building or log paths:
  every traversal, smuggling, splitting and injection payload was already
  refused. The slowloris hole was the one real finding, and it was in a timeout
  the documentation described correctly and the code implemented differently.

## [0.8.2] - 2026-10-13

Hardening step 3: an optional seccomp-bpf sandbox.

### Added

- **`seccomp`** (and `--seccomp on|off`): after start-up the process is limited
  to an allow list of 79 syscalls; anything else kills it. Off by default.
  The list was collected by tracing the server through every feature -- static
  files, listings, ranges, conditional requests, the status endpoint, reload,
  log reopen, statistics, pipelining, the keep-alive timeout and a graceful
  shutdown -- and then reviewed entry by entry. `make test-seccomp` replays that
  workload with the filter armed, so a missing entry fails the test instead of
  showing up as a production crash.
- Deliberately absent from the list: `execve`/`execveat`, `ptrace`,
  `mount`/`umount`, `reboot`, `kexec_load`, `init_module`/`finit_module`,
  `bpf`, `process_vm_*`, and every `setuid`/`setgid` family. The privilege drop
  happens before the filter goes on, so afterwards the process cannot change
  identity again.
- `PR_SET_NO_NEW_PRIVS` is set and the filter is installed with
  `SECCOMP_FILTER_FLAG_TSYNC`, so it covers every worker thread. A kernel that
  cannot install it is a warning, not a start-up failure.

### Fixed

- **The filter allowed every syscall while reporting itself active.** The kill
  return was emitted before the allow return, so falling through the last
  comparison landed on ALLOW: a silent no-op sandbox that still logged "seccomp
  filter active". Found by a test that asserts a forbidden syscall kills the
  process -- `ptrace` returned 0 with the filter armed. The kill return now
  comes first and each comparison jumps forward to the allow return. Verified:
  the same probe now dies with SIGSYS.
- `readlinkat` was missing from the list. The symptom was not a crash but a
  hang: a sanitized build spins on `sched_yield()` forever at shutdown, because
  a sanitizer resolves addresses to file names through
  `readlinkat("/proc/self/exe")`. The release build was unaffected, which is
  why it took an ASan run to find.
- `_Static_assert` pins the allow list below 255 entries: a cBPF jump distance
  is a `uint8_t`, and an oversized list would silently produce wrong distances.

### Known limitation

- LeakSanitizer (on by default in ASan) cannot complete its stop-the-world
  under a seccomp filter: the process spins on `sched_yield()` at exit instead
  of shutting down. This is a sanitizer limitation, not a server bug -- the same
  binary with `detect_leaks=0` exits in 0.01s, and the unsanitized build exits
  in 0.00s with the filter armed. `tests/seccomp_test.py` sets `detect_leaks=0`
  for sanitizer builds and documents why.

### Tests

- `test_seccomp` (3045 checks): the allow list is well formed, unique, sized
  under the cBPF limit, contains every syscall the serving path needs, and
  contains none of the dangerous ones.
- `tests/seccomp_test.py`: a full sandboxed workload (17 checks) plus the
  enforcement proof, where a probe built from the same filter source must die
  with SIGSYS on a forbidden syscall.

## [0.8.1] - 2026-10-13

Hardening step 2: one client can no longer take the whole server.

### Added

- **`max_conns_per_ip`**: a cap on how many connections one client address may
  hold at once. Without it, a single host opening `max_connections` sockets
  denies service to everyone else, which is the cheapest denial-of-service
  there is. `0` (the default) leaves it unlimited.
- **`connlimit_slots`**: the size of the tracking table (default 8192).
- The tracking table is **shared by every worker**, so the limit counts the
  client rather than one worker's share of it. Proven: with two workers and
  `max_conns_per_ip = 4`, exactly 4 of 30 connections are accepted -- not 8.

### Design

- Sharded, direct-mapped, lock-free. A key hashes into one slot in each of 16
  shards; the matching slot is used and a collision means the address is not
  tracked, which fails **open** (the connection is allowed) rather than closing
  somebody else's connections. The reservation uses a compare-and-swap loop, so
  two workers can never both slip past the limit. The refusal counter and every
  slot are C11 atomics: nothing on the accept path takes a lock.
- The reservation is released in `dx_conn_reset()`, the single place a slot is
  recycled, not in each caller. That is deliberate: while auditing this feature
  the release had been left to the caller in one error path (the `epoll_ctl`
  failure path), which leaked a reservation and banned that address
  permanently. Putting it where the slot is recycled makes the omission
  impossible.
- A stray release cannot underflow the counter (which would deny an address
  forever), and IPv4 and IPv4-mapped IPv6 peers are counted separately, because
  they are different sockets -- normalising them would let a client use both
  forms to double its allowance.

### Tests

- `test_connlimit` (52 checks) covers the exact limit, the no-underflow rule,
  IPv6 keys, the fail-open cases, and a concurrency test where 8 threads race
  for an allowance of 50: exactly 50 of 4000 attempts are granted.
- A regression test covers the shared-counter behaviour across workers, the
  release on close (20 sequential connections all served with a limit of 2) and
  the off-by-default case.

## [0.8.0] - 2026-10-13

Hardening, and the first step of it: the server can now see its own descriptors.

### Added

- **Runtime descriptor accounting** (`src/fdcount.c`). Every descriptor the
  server opens is counted under a tag (`conn`, `file`, `cache`, `listen`,
  `epoll`, `wakeup`, `log`, `misc`), and every close decrements it. Two views
  of the truth:
  - `fds_believed` -- what the server's accounting says is open;
  - `fds_actual` -- what the kernel reports in `/proc/self/fd`.
  Both are exposed in the `/status` JSON as a `descriptors` object. Their
  difference is a constant set of process-wide descriptors, so **if the gap
  grows, descriptors are leaking**.
- Proven on the exact bug class it targets: injecting an untracked
  `open("/dev/null")` into the serving path makes the gap grow 205 -> 405, and
  the regression test fails. Reverting the fix for the v0.7.3 descriptor leak
  makes the absolute-count test fail (9 -> 308). The two checks are
  complementary: one catches descriptors the accounting never saw, the other
  catches tracked ones that are never released.

### Changed

- The status endpoint JSON gained a `descriptors` object. Nothing else about
  the format changed.

## [0.7.3] - 2026-10-13

Fewer syscalls per request, and a descriptor leak that had been there all along.

### Fixed

- **A keep-alive connection leaked one file descriptor per request.** The
  descriptor was released only when the connection closed, so a client asking
  for a thousand files on a single connection held a thousand descriptors open
  until it disconnected. That exhausts `RLIMIT_NOFILE` long before the
  connection limit is reached, and it is why a low `ulimit -n` showed up as
  requests failing after a few hundred. The descriptor is now released as soon
  as the response body is on the wire (`conn_after_response`). Proven: 300
  requests on one connection went from 308 open descriptors to 9.
- Two defects in the new cache found while auditing it, before release: a
  cached descriptor was closed by the connection that had just inserted it
  (the cache then handed out a stale descriptor number), and `dx_fcache_put`
  refused to fill a slot that held a different, unpinned path.

### Added

- **Per-worker open-file cache** (`open_file_cache`, `open_file_cache_valid`).
  Off by default, because it trades staleness for speed: within the validity
  window a file replaced on disk is still served from the cached descriptor.
  When enabled it removes the `openat2()` and `fstat()` from every repeated
  request. Direct-mapped and lock-free (one per worker), and a descriptor is
  pinned while a transfer uses it, so it is never closed out from under a live
  `sendfile()`.
- **`epoll_ctl` is skipped when the interest mask does not change.** A request
  that arrives, is answered and is sent without the socket ever blocking used
  to issue a redundant `EPOLL_CTL_MOD` on every turn.

### Performance (measured with `strace -c`, 1,000 keep-alive requests for 1 KiB)

| | syscalls per request | breakdown |
|---|---|---|
| before | 7 | openat2, fstat, read, sendto, sendfile, epoll_ctl, epoll_wait |
| after (cache on) | 4 | read, sendto, sendfile, epoll_wait |

`openat2` and `fstat` drop from one per request to one at startup; `epoll_ctl`
drops from one per request to two per 1,000.

## [0.7.2] - 2026-10-13

The per-connection memory breakthrough: an idle keep-alive connection now costs
~1.2 KiB instead of ~9.2 KiB.

### Changed

- **The HTTP header table is pooled, not embedded.** Every connection used to
  carry a 256-entry header array (8 KiB) inside its slot, whether or not a
  request was in flight. That array is now a caller-owned table acquired from a
  pool together with the read buffer and returned the moment the connection
  goes idle. Measured on this machine, 10,000 idle keep-alive connections now
  cost 12 MiB of RSS instead of 90 MiB -- the documented "user-space memory per
  idle connection <= 1 KiB" target is met for the first time (marginal cost
  ~1.0 KiB; ~1.2 KiB including the fixed baseline).
- `dx_http_parser_init_ex()` takes the table and its capacity; the parser never
  allocates. A request with more headers than the table holds is answered 431
  rather than read past it. `dx_http_parser_init()` keeps its old signature and
  means "no header storage", which is only useful for request-line-only callers.

### Fixed

- Two defects found while auditing this change, both caught by the test suite
  before release:
  - `dx_conn_release_scratch()` cleared the header-table pointer without
    returning the pool slot, leaking one 8 KiB table per directory listing and
    breaking the parser for that request.
  - The fuzz target stopped exercising header parsing (a zero-capacity table
    makes every header-bearing request 431); it now owns a real table.

## [0.7.1] - 2026-10-13

Tuning that actually reaches the kernel, plus a host pre-flight.

### Added

- **`--doctor`**: a pre-flight that checks the host before start-up and exits
  non-zero when something would block it. It reports `RLIMIT_NOFILE` against
  `workers x max_connections`, `net.core.somaxconn` against the backlog, whether
  the port is free (it tries to bind it), whether the document root is usable,
  and whether the configured privilege-drop user and group exist -- each with
  the exact number that is wrong and the command to fix it.

### Fixed

- **`tcp_defer_accept`, `tcp_fastopen`, `socket_rcvbuf` and `socket_sndbuf`
  were accepted by the configuration and then silently ignored.** They are now
  applied to every listener before `listen()`. An option the kernel refuses
  (for example `TCP_FASTOPEN` on a host whose `net.ipv4.tcp_fastopen` lacks the
  server bit) is reported as a warning and the server keeps running: a missing
  tunable is never fatal.
- **The automatic `RLIMIT_NOFILE` tuning documented in the README did not
  exist.** The soft limit is now raised towards the hard one at start-up, and
  when even that is below `workers x max_connections`, `max_connections` is
  lowered with a warning that names both numbers. Before this, a host with the
  usual 1024 soft limit refused every connection past roughly a thousand with
  no explanation.

## [0.7.0] - 2026-10-13

Measurable: the benchmark harness, a real set of measurements, and the first
optimization.

### Added

- **`bench/tools/idle_conns.c`** (scenario S5): holds N idle TCP connections,
  optionally after one request each, polls for connections the server dropped,
  and prints a summary. It supports `-s ip,ip,...` to rotate source addresses
  so a single host can exceed one IP's ~28k ephemeral port range. Verified with
  10,000 connections held with zero drops.
- **`bench/make_files.sh`** creates the 1k/10k/100k/1m/100m payloads (and a
  small index) that every scenario uses.
- **`bench/run_wrk.sh`**, **`bench/run_all.sh`** and **`bench/compare_reference.sh`**
  drive the scenarios from the README; `make bench` prepares the payloads and
  prints the commands, `make stress` the long-hold commands.
- **`bench/mixed.lua`** (weighted mixed file sizes), **`bench/reference.conf`**
  (a reference configuration for the comparison) and
  **`bench/slowloris.py`** (slow-connection resilience, scenario S8).
- **`bench/py_loadgen.py`**: a minimal keep-alive load generator for machines
  without `wrk`. It reports a lower bound only and says so.
- Measured results in the README's benchmark tables (hardware, kernel,
  compiler, commit hash, and the numbers), replacing the `TBD` placeholders
  where a measurement was possible on this machine.

### Changed

- **The `Date` response header is served from a one-second cache.** It used to
  run `gmtime_r()` + `strftime()` on every response; the value only changes
  once per second, so it is rendered once per second and copied afterwards. The
  cache is mutex-guarded and only ever holds a complete fixdate, so a worker
  can never copy a half-written value (`dx_http_date_cached()`, unit tested).

### Notes

- The latency columns (p50/p99/p99.9) remain `TBD`: they need `wrk`/`wrk2`,
  which are not installed on this machine. The measured throughput is a lower
  bound because the Python client becomes the bottleneck first.
- Edge-triggered `epoll`, the open-file cache and `writev()` for small files
  remain planned for a later pass; the `Date` cache was the change that fits
  the existing level-triggered design without a behavioural risk.

## [0.6.3] - 2026-10-12

Re-audit verdict on v0.6.2: 33 of the 40 third-round fixes were correct; the
pipelining de-recursion had broken two things, and three smaller defects
remained. All are fixed here.

### Fixed

- **A keep-alive connection was never IDLE, so `keepalive_timeout` was never
  applied.** The pipelining de-recursion had left an unreachable tail in
  `conn_after_response()`: every idle connection kept its read buffer and was
  closed by the *header* timeout instead (reproduced: `--keepalive-timeout 5s
  --header-timeout 2s` closed at ~1.6 s with "header timeout" in the log). The
  idle path is reachable again: the buffer goes back to the pool and the
  keepalive timeout is armed.
- **Pipelined requests could stall or be answered with a 408 after a partial
  write.** When `send()` hit EAGAIN the remaining buffered requests were parked
  in `conn->hold`, and the de-recursion had removed the code that parsed them
  once the response drained: they sat in the read buffer until the stale header
  timeout fired (a 408 for a valid request), or the buffer filled from the
  socket and the connection was killed with an RST. The event loop now parses
  restored pipelined requests explicitly, and a pipelined burst of 6000 and of
  20000 requests is answered completely (verified with a client that stops
  reading for 5 s).
- `dx_log_access()` read `log->out` outside the mutex (the same reopen race
  that `dx_log_write()` already fixed). Both loggers hold the mutex across the
  whole write now.
- The reload path wrote the shared `Cache-Control` buffer that workers read on
  the hot path without a lock, so a concurrent SIGHUP could emit a header
  mixed from old and new content. Two buffers are flipped on reload instead.
- A **double `close(fd)`** in `tests/unit/test_mime.c` (the audit's own cleanup
  had added a second one).
- The Makefile had no header dependencies for the unit-test, example and fuzz
  binaries, so editing `src/*.h` left them stale. Tests and fuzz binaries now
  list the headers as prerequisites; examples compile per object with
  `-MMD -MP`.
- `bytes_out` was counted twice on the pread() and in-memory-body paths (the
  park path and `conn_flush()` both counted the same bytes).

### Added

- Regression tests for both regressions: an idle keep-alive connection must be
  closed by the keepalive timeout where it differs from the header timeout, and
  a pipelined burst of 1500/6000 requests must be answered completely with no
  request-buffer exhaustion.

## [0.6.2] - 2026-10-12

Fourth audit round: all 40 findings verified as real and fixed.

### Fixed

Critical

- **SIGPIPE killed the server in single-worker mode.** `SIG_IGN(SIGPIPE)` was
  installed only in `server_run_multi()`, so the default mode was unprotected:
  `sendfile()` has no `MSG_NOSIGNAL`, and a log destination whose reader had
  exited killed the process on the next log line (reproduced, exit 141). It is
  now installed once in `dx_server_run()`, before either mode starts.
- **`log_level` was parsed, validated, printed and documented but ignored.**
  `dx_server_config_t` had no such field and `logs_open()` hardcoded
  `DX_LOG_INFO`, so `--log-level error` still emitted `[info]` lines and the
  whole debug layer was dead. It is wired through now.
- **`-U_FORTIFY_SOURCE` cancelled `-D_FORTIFY_SOURCE=2`**, so every build was
  made without fortify while the Makefile claimed otherwise.
- **`make coverage` always failed**: the recipe prefixed `$(TESTS)/` onto
  `TEST_SRCS`, which already contained full paths.
- **`make test-asan` built the binaries and never ran them** (and `fail` was
  never set), so the target always "passed" while running zero tests.

High

- `examples/03`: a full read buffer made `read(fd, buf, 0)` return 0, which was
  mistaken for a peer FIN (echo dropped data under load).
- `examples/04`: `strstr()` scanned past `len` into uninitialised stack bytes
  (no NUL terminator), and `Content-Length: 15` did not match the 14-byte body.
- `tests/c10k_test.py` checked the *hard* nofile limit instead of the *soft*
  one (the one actually enforced), so a default `1024` made the suite fail
  after ~1020 connections. The soft limit is now raised where possible.
- `tests/smoke_test.py` defaulted to a binary path that is never built and had
  no existence guard.
- `tests/unit/testing.h`: `CHECK_NE` passed 5 arguments to a format with 6
  conversions (a failing check printed a counter as a `char*`). It compiles
  clean only because no test uses it yet.
- **The pipelining path recursed once per buffered request** through
  `conn_finish -> conn_after_response -> conn_process -> conn_dispatch ->
  conn_serve_static -> conn_finish`. In -O0/ASan/TSan builds (where tail calls
  are not eliminated) a few hundred pipelined requests exhausted the stack:
  reproduced as a stack-overflow at ~10 frames with a 256 KiB stack. The chain
  is iterative now; 3000 pipelined requests are served in an ASan build with a
  256 KiB stack.

Medium

- The systemd unit failed to start when `/var/log/dantrix` did not exist
  (`LogsDirectory` replaces the hard `ReadWritePaths`).
- `logrotate` refused to rotate (no `su` directive for a non-root directory).
- The Docker image could link stale host-architecture objects (`make clean`
  before `make release`).
- `make coverage` / `test_*` / `fuzz_*` / `examples` had no header dependencies,
  so editing a header left them stale.
- `EPOLLERR|EPOLLHUP` dropped a response that was already buffered.
- `bytes_out` was counted twice on the pread() and in-memory-body paths.
- The `SIGUSR1` log reopen raced `dx_log_write()`'s unlocked `log->out` read.
- A concurrent `SIGHUP` could mix old and new `Cache-Control` bytes.
- An exported `CFLAGS` silently defeated `-O2 -DNDEBUG` in the release build.
- CI ran the fuzz harness without sanitizers, documented the wrong port, and
  hardcoded the ASan unit-test list (a new test silently missed the job).
- A `206` could be sent without its mandatory `Content-Range`.
- The example servers exited on transient `accept()` errors and kept servicing
  a dead connection after a write error.
- Unit-test defects that masked regressions: `CHECK(c == c)` for the slot
  recycle, a MIME override mapped to the built-in value, and a 431 fixture with
  a wrong length.

Low / latent

- FILE* leak when the access log failed to open; a stale connection timer left
  armed when `epoll_ctl` failed; the loopback check rejected IPv4-mapped IPv6
  peers; `keepalive_timeout = 0` disabled the idle timeout entirely (now
  rejected); `workers auto` bypassed the 1024 cap; `openat2` blocked by policy
  produced a silent site-wide 403 (now warned at start-up); the mime.types
  1 MiB cap truncated silently (now warned); dead parser fields removed; OOB
  reads on unit-test failure paths; fd/join/memcpy hygiene in the unit tests;
  `read_response()` now survives a timeout instead of aborting the suite; the
  fuzz harness lost its dead length computation; example 02's thread-count
  command pointed at a path that does not exist; a relative `pid_file` is now
  resolved before the daemon `chdir("/")`s.

### Added

- `dx_fs_openat2_blocked()` so a seccomp-blocked `openat2()` warns at start-up
  instead of silently serving 403s.
- `dx_config_set()` (key and value separately), which removed the intermediate
  "key=value" string that truncated long path values.
- The CI checks the ASan unit-test list against the Makefile wildcard, so a
  newly added test cannot silently miss the job.

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
  class), `"<mtime-hex>-<size-hex>"` `ETag` formatting, weak
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
