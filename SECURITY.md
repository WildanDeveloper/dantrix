# Security Policy

## Status: pre-alpha, unaudited

Dantrix has **not** had an independent security audit. It does not support TLS.
Do not expose it directly to the internet for sensitive data.

What it *does* have, as of v0.8.3:

- a documented threat model (below);
- build-time hardening and a strict parser;
- an optional seccomp-bpf sandbox;
- **a test suite that verifies the security claims empirically**:
  `make test-security` runs 30+ checks that attack a live server the way an
  untrusted client would. A claim that cannot be tested is listed as
  *unverified* in the table rather than assumed true.

## Reporting a vulnerability

Open a private security advisory on GitHub
(`Security` → `Advisories` → `Report a vulnerability`) rather than a public
issue. Include the version, a minimal reproduction, and what you expected.

There is no bounty. This is a learning project; a clear report is the reward.

## Threat model

**Untrusted:** every byte arriving over the network, including the request line,
headers, body, and the client's timing.

**Trusted:** the operator, the configuration file, and the contents of the
document root. Anything inside the document root is treated as **public**: if it
can be reached, it can be downloaded. Do not put secrets in the document root.

**Trust boundary:** the HTTP parser and the `path` module. Everything else in the
server may assume their output is validated.

**Explicitly out of scope for this version:**

- TLS (not implemented; terminate at a reverse proxy);
- HTTP/2 and HTTP/3;
- protecting a document root that a *local* unprivileged user can modify while
  the server runs (that user could swap a file between the check and the open --
  the server has no defence against a hostile local filesystem);
- denial of service from a source that can saturate the network link itself.

## Verified claims

Run `make test-security` to reproduce every line below. The column says how the
claim is checked.

| Claim | How it is verified | Result |
|---|---|---|
| `../`, `%2e%2e`, double-encoded, backslash, `%00` and dot-segment tricks cannot escape the document root | 12 traversal payloads sent to a live server with a secret file outside the root; none leak | verified |
| A symlink pointing outside the root is refused (`follow_symlinks = false`) | a real symlink is created and requested | verified |
| A legitimate file is still served (the refusals are not blanket) | same server, normal path | verified |
| `.env`, `.git/config` and other dot segments are not served | 3 paths requested, contents checked absent | verified |
| `/.well-known/` remains reachable (documented exception) | path requested, content returned | verified |
| Duplicate or conflicting `Content-Length` is rejected | 2 payloads, both answered `400` | verified |
| `Transfer-Encoding` (alone and with `Content-Length`) is rejected | 2 payloads, both refused | verified |
| Bare LF line endings and `Header : value` (space before colon) are rejected | 2 payloads, both refused | verified |
| CR/LF in a request path cannot inject a response header | 3 payloads whose decoded form contains CRLF, requested so the redirect path would build a `Location` from it; no injected header appears | verified |
| A redirect `Location` never begins with `//` (no open redirect) | 4 targets including `//sub` and `/%2fsub`; every `Location` starts with a single `/` | verified |
| `Content-Length` overflow, negative, hex, signed and multi-value forms are refused | 6 payloads, all answered `400` | verified |
| Many overlapping ranges are not expanded into a multipart body | 500 ranges in one header; a plain `200` is returned | verified |
| Oversized request line, header count and body are refused | limits lowered, then exceeded; `414`, `431` and a refusal | verified |
| `server_tokens = off` removes the version from the response | header checked absent | verified |
| A client cannot forge a log line with CR/LF | request with CRLF in the header block; every log line starts with the client address and the request produces exactly one line | verified |
| After start-up the process runs as the configured user with `no_new_privs` | server started as root with `--user nobody`, then `/proc/<pid>/status` read | verified (needs root) |
| The seccomp filter is active on the running process | `Seccomp: 2` in `/proc/<pid>/status` | verified |
| A syscall outside the seccomp allow list kills the process | probe built from the same filter source; dies with `SIGSYS` | verified |
| The kernel enforces the document root atomically (`openat2(RESOLVE_BENEATH)`) | no check-then-open pattern exists in the serving path; the fallback walk uses `O_NOFOLLOW` per segment. The kernel API itself is not something this test suite can falsify | verified by construction, not falsified |
| A slowloris client (one byte at a time) cannot hold a connection open | a 0.4s drip with `header_timeout=1s` is closed after 1.6s; the same drip held the connection for 12.3s before v0.8.3 | verified |
| A slow but *complete* request inside the window is still served | the same request sent in two halves 0.3s apart answers `200` | verified |
| The header timeout is a deadline, the body timeout a gap | documented in the timeout table; the difference is deliberate, because they protect different things (a stuck header phase vs. a slow genuine upload) | verified by test |
| Build hardening is in effect | the *binary* is inspected, not the Makefile: `readelf` confirms PIE, non-executable `GNU_STACK`, `GNU_RELRO` and `BIND_NOW` (full RELRO); `nm` confirms fortified `*_chk` symbols and the stack protector | verified |

## Known limitations

1. **No TLS.** Terminate at a reverse proxy, and bind Dantrix to loopback.
2. **`open_file_cache` serves stale content.** Within the validity window a file
   replaced on disk is still served from the cached descriptor. Keep the window
   short on a host that swaps files in place, or leave the cache off (the
   default).
3. **No `chroot`.** The document root is enforced by `openat2`, not by a
   filesystem namespace.
4. **The local filesystem is trusted.** A local user who can modify the document
   root can influence what is served.
5. **LeakSanitizer cannot run under the seccomp sandbox** (it spins at exit).
   This is a sanitizer limitation, documented in `tests/seccomp_test.py`.
6. **One process, many threads.** A memory-safety bug in a worker kills the
   whole server; there is no supervisor-process isolation. Run it under a
   supervisor with `Restart=on-failure`.
7. **`max_conns_per_ip` is not a rate limit.** It bounds *concurrent* connections
   from one address. A client that opens and closes connections in a loop is
   bounded only by the accept path's speed, not by this feature.

## What is deliberately not implemented

- **Multi-range requests** (`Range: bytes=0-10,20-30`): the server ignores the
  header and sends a full `200`. Serving several ranges means building a
  multipart body, and the overlapping-range attack class makes that a poor
  trade for a static file server.
- **`Transfer-Encoding: chunked`**: rejected outright. A static file server has
  no reason to accept a request body it cannot frame.
- **Directory listings by default**: disabled, because a listing discloses the
  file names a client has not guessed.
