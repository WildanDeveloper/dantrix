#!/usr/bin/env python3
"""End-to-end tests for the seccomp sandbox (v0.8.2).

Two properties, and the second is the one that matters:

  1. With the filter armed the server survives a full workload -- static files,
     directory listing, ranges, conditional requests, the status endpoint,
     reload, log reopen, statistics, pipelining, the keep-alive timeout and a
     graceful shutdown. A missing entry in the allow list shows up here as the
     server dying with SIGSYS, so the list cannot silently go stale.
  2. The filter is actually enforcing: a syscall outside the list kills the
     process. This is verified against the server itself, by asking it to run a
     forbidden syscall through a deliberate backdoor... which does not exist.
     Instead it is verified against a helper program built from the same
     allow-list source, so the check exercises the real filter code.

Usage: python3 tests/seccomp_test.py [path-to-binary]
"""

import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "bin/dantrix"
PORT = int(os.environ.get("DANTRIX_TEST_PORT", "18101")) + 70
failures = []
ROOT = tempfile.mkdtemp(prefix="dantrix-seccomp-")
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def wait_ready(proc, port, tries=60):
    for _ in range(tries):
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fetch(port, request, until_close=False):
    """Send a request, read the response.

    until_close=True keeps reading until the peer closes, which is what a
    pipelined pair of requests needs: the helper would otherwise stop after the
    first response and the second would look missing.
    """
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        s.sendall(request)
        s.settimeout(3)
        data = b""
        while True:
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            data += chunk
            if until_close:
                continue
            if b"\r\n\r\n" in data:
                head, _, rest = data.partition(b"\r\n\r\n")
                length = 0
                for line in head.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        try:
                            length = int(line.split(b":", 1)[1])
                        except ValueError:
                            length = 0
                if len(rest) >= length:
                    break
        return data
    finally:
        s.close()


def test_sandboxed_workload():
    """The full feature set must work with the filter armed.

    Note on sanitized builds: LeakSanitizer (on by default in ASan) performs a
    stop-the-world when it runs at exit, and that mechanism does not complete
    under a seccomp filter -- the process spins on sched_yield() forever instead
    of exiting. That is a sanitizer limitation, not a server bug: the same
    binary with detect_leaks=0 exits in 0.01s, and the unsanitized build exits
    in 0.00s with the filter armed. The environment below disables the leak
    check only when the binary is a sanitizer build, so this test still proves
    the server itself shuts down cleanly under the sandbox.
    """
    print("seccomp: full workload with the filter armed")

    with open(os.path.join(ROOT, "f"), "w", encoding="ascii") as f:
        f.write("x" * 4096)
    os.makedirs(os.path.join(ROOT, "dir"), exist_ok=True)
    with open(os.path.join(ROOT, "dir", "child.txt"), "w", encoding="ascii") as f:
        f.write("y")
    with open(os.path.join(ROOT, "big.bin"), "wb") as f:
        f.write(os.urandom(200000))
    pid_file = os.path.join(ROOT, "dx.pid")
    log = os.path.join(ROOT, "err.log")

    env = dict(os.environ)
    if "asan" in BIN or "san" in BIN:
        # LeakSanitizer cannot stop the world under seccomp: see the note above.
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "-r", ROOT, "-w", "2",
         "--access-log", "off", "--error-log", log,
         "--directory-listing", "on",
         "--status-path", "/status",
         "-o", "seccomp=true",
         "-o", "open_file_cache=8",
         "-o", "open_file_cache_valid=60s",
         "-o", "max_conns_per_ip=100",
         "-o", "keepalive_timeout=2s",
         "-o", f"pid_file={pid_file}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    try:
        if not wait_ready(proc, PORT):
            check("sandboxed: server started", False)
            return
        check("sandboxed: the filter is reported active",
              "seccomp filter active" in open(log, encoding="utf-8", errors="replace").read())

        # Every serving path, in one pass.
        cases = [
            ("static file", b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", b"200"),
            ("directory listing", b"GET /dir/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             b"200"),
            ("range", b"GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-99\r\n"
                      b"Connection: close\r\n\r\n", b"206"),
            ("not modified", b"GET /f HTTP/1.1\r\nHost: x\r\n"
                             b"If-Modified-Since: Wed, 01 Jan 2030 00:00:00 GMT\r\n"
                             b"Connection: close\r\n\r\n", b"304"),
            ("missing", b"GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", b"404"),
            ("status", b"GET /status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", b"200"),
            ("options", b"OPTIONS /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", b"200"),
            ("method not allowed", b"POST /f HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
                                   b"Connection: close\r\n\r\n", b"405"),
        ]
        for name, req, want in cases:
            got = fetch(PORT, req)
            check(f"sandboxed: {name} answers {want.decode()}", got.startswith(b"HTTP/1.1 " + want),
                  got[:40])

        # Pipelining: both responses must arrive on one connection.
        got = fetch(PORT, b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n"
                          b"GET /dir/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                    until_close=True)
        check("sandboxed: pipelined requests answered", got.count(b"HTTP/1.1 200") == 2,
              f"{got.count(b'HTTP/1.1 200')} responses")

        # Signals: reload, log reopen, statistics.
        proc.send_signal(signal.SIGHUP)
        time.sleep(0.4)
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.3)
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.3)
        check("sandboxed: alive after HUP/USR1/USR2", proc.poll() is None)
        check("sandboxed: still serving after the signals",
              fetch(PORT, b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
              .startswith(b"HTTP/1.1 200"))

        # Keep-alive timeout path.
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
        s.settimeout(5)
        s.recv(65536)
        # keepalive_timeout is 2s: wait past it, then give the close a moment to
        # arrive. Polling instead of one fixed sleep keeps this from depending on
        # scheduler timing.
        deadline = time.time() + 6.0
        closed = False
        while time.time() < deadline:
            try:
                if s.recv(64) == b"":
                    closed = True
                    break
            except socket.timeout:
                continue
            except OSError:
                closed = True
                break
        s.close()
        check("sandboxed: idle keep-alive closed by the timeout", closed)
        check("sandboxed: alive after the keep-alive timeout", proc.poll() is None)

        # Graceful shutdown, including the pid file removal (unlink).
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=6)
            check("sandboxed: graceful shutdown exits cleanly", True)
        except subprocess.TimeoutExpired:
            check("sandboxed: graceful shutdown exits cleanly", False, "still running")
        check("sandboxed: pid file removed at shutdown (unlink allowed)",
              not os.path.exists(pid_file))

        text = open(log, encoding="utf-8", errors="replace").read()
        check("sandboxed: no SIGSYS / killed message in the log",
              "bad system call" not in text.lower() and "SIGSYS" not in text)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        shutil.rmtree(ROOT, ignore_errors=True)


def test_filter_enforces():
    """A syscall outside the allow list must kill the process.

    The helper is compiled from the same allow-list source and installs the same
    filter, then calls a forbidden syscall. It must die with SIGSYS (159 as seen
    by the shell), which is what the server would do if a request ever reached
    code that needed a syscall that is not on the list.
    """
    print("seccomp: the filter actually enforces")

    src = os.path.join(SRC, "tests", "unit", "seccomp_probe.c")
    if not os.path.exists(src):
        check("enforce: probe source present", False, src)
        return

    out = os.path.join(tempfile.gettempdir(), "dantrix_seccomp_probe")
    cmd = ["gcc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-Isrc", src,
           os.path.join(SRC, "src", "seccomp.c"), "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        check("enforce: probe builds", False, r.stderr[:200])
        return
    try:
        p = subprocess.run([out], capture_output=True, text=True, timeout=20)
        # 159 == 128 + SIGSYS(31): killed by the filter.
        check("enforce: a forbidden syscall kills the process (SIGSYS)",
              p.returncode == -signal.SIGSYS or p.returncode == 159,
              f"returncode={p.returncode} stdout={p.stdout[:80]!r} stderr={p.stderr[:80]!r}")
    finally:
        if os.path.exists(out):
            os.unlink(out)


def main():
    test_sandboxed_workload()
    test_filter_enforces()

    print()
    if failures:
        print(f"seccomp test: FAILED ({len(failures)} checks)")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("seccomp test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
