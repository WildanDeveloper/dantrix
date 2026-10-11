#!/usr/bin/env python3
"""Regression tests that need their own server process.

These cover defects that a single shared server cannot demonstrate:

  * BUG-01  the timing wheel kept a stale minimum and spun the event loop
  * BUG-05  a rejected connection kept its descriptor
  * BUG-07  the shutdown drain skipped every connection whose slot index is
            above max_conns (which is all of them, with the production chunk)
  * BUG-14  descriptor exhaustion must back off without spinning or sleeping
            in the event loop

Usage: python3 tests/regression_test.py [path-to-binary]
"""

import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "bin/dantrix"
PORT = int(os.environ.get("DANTRIX_TEST_PORT", "18101"))
failures = []
ROOT = tempfile.mkdtemp(prefix="dantrix-regression-")
with open(os.path.join(ROOT, "f"), "w", encoding="ascii") as _f:
    _f.write("x")


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


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat", "r", encoding="ascii") as f:
            fields = f.read().rsplit(") ", 1)[1].split()
        return int(fields[11]) + int(fields[12])  # utime + stime
    except (OSError, IndexError, ValueError):
        return None


def open_fd_count(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return None


def test_idle_cpu_after_partial_request():
    """BUG-01: a lazy refresh inside one bucket used to leave a past deadline in
    an empty bucket, so next_timeout_ms() answered 0 forever and epoll_wait(0)
    spun at full speed."""
    port = PORT
    # The header clock is deliberately short: the stale minimum it leaves
    # behind is a couple of seconds away, so the spin shows up within the test
    # instead of a minute later.
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "--root", ROOT,
         "--header-timeout", "2s", "--keepalive-timeout", "1s"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        if not wait_ready(proc, port):
            check("idle CPU: server started", False)
            return
        # Split requests drive the lazy refresh path (a raise inside the same
        # one-second bucket, followed by a detach from another bucket). One
        # attempt hits that window with a probability of 1 - gap/1s, so a few
        # spread gaps make it near certain.
        for delay in (0.05, 0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.1, 0.2, 0.3, 0.4):
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=5)
                s.sendall(b"GET /f HTTP/1.1\r\n")
                time.sleep(delay)
                s.sendall(b"Host: x\r\n\r\n")
                s.settimeout(3)
                s.recv(65536)
                s.close()
            except OSError:
                pass
        time.sleep(4)  # past the stale deadline
        before = cpu_ticks(proc.pid)
        time.sleep(3)
        after = cpu_ticks(proc.pid)
        if before is None or after is None:
            check("idle CPU stays near zero after a partial request", False, "no /proc")
            return
        ticks = after - before
        print(f"        cpu ticks over 3 idle seconds: {ticks}")
        check("idle CPU stays near zero after a partial request", ticks < 25,
              f"{ticks} ticks over 3s (100 ticks = one busy core-second)")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_rejected_connection_closes_fd():
    """BUG-05: conn_open() returned -1 on the two reject paths without closing
    the descriptor it had just accepted, leaking one fd per rejected client."""
    port = PORT + 1
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "--root", ROOT, "--max-conns", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    held = []
    try:
        if not wait_ready(proc, port):
            check("fd leak: server started", False)
            return
        # The readiness probe releases its slot only after the server saw the
        # EOF, so retry until this keep-alive request really holds the one slot.
        holder = None
        for _ in range(30):
            try:
                cand = socket.create_connection(("127.0.0.1", port), timeout=5)
                cand.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
                cand.settimeout(3)
                if cand.recv(65536).startswith(b"HTTP/1.1 200"):
                    holder = cand
                    break
                cand.close()
            except OSError:
                pass
            time.sleep(0.2)
        if holder is None:
            check("fd leak: the slot could be taken", False)
            return
        held.append(holder)
        time.sleep(0.3)

        before = open_fd_count(proc.pid)
        clients = []
        for _ in range(30):
            c = socket.create_connection(("127.0.0.1", port), timeout=5)
            c.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
            clients.append(c)  # left open on purpose
        time.sleep(1.0)
        after = open_fd_count(proc.pid)
        for c in clients:
            c.close()
        if before is None or after is None:
            check("rejected connections do not leak descriptors", False, "no /proc")
            return
        check("rejected connections do not leak descriptors", after == before,
              f"fds {before} -> {after} after 30 rejections")
    finally:
        for s in held:
            s.close()
        proc.send_signal(signal.SIGTERM)
        try:
            _, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            err = b""
        logged = err.count(b"connection rejected")
        check("the rejections were logged", logged >= 25,
              f"{logged} log lines for 30 rejected clients")


def test_shutdown_drain_reaches_every_connection():
    """BUG-07: slots are handed out from the top of the free list, so with
    --max-conns 2 the live connections sit at indices 1023 and 1022. The drain
    loop scanned [0, max_conns), found nothing, closed nothing and lost the
    access log line of every connection."""
    for max_conns, port in ((2, PORT + 2), (2000, PORT + 3)):
        log = tempfile.NamedTemporaryFile(prefix="dantrix-drain-", suffix=".log", delete=False)
        proc = subprocess.Popen(
            [BIN, "-b", "127.0.0.1", "-p", str(port), "--root", ROOT,
             "--max-conns", str(max_conns), "--shutdown-timeout", "1s",
             "--keepalive-timeout", "30s", "--access-log", log.name,
             "--log-format", "common"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sock = None
        try:
            if not wait_ready(proc, port):
                check(f"drain with --max-conns {max_conns}: server started", False)
                continue
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
            sock.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.3)
            try:
                sock.recv(65536)
            except OSError:
                pass
            time.sleep(0.2)
            proc.send_signal(signal.SIGTERM)
            _, err = proc.communicate(timeout=15)
            sock.settimeout(3)
            try:
                sock.recv(100)
            except OSError:
                pass
            sock.close()
            sock = None
            text = err.decode(errors="replace")
            complete = re.search(r"closed (\d+)\)", text)
            closed = int(complete.group(1)) if complete else -1
            check(f"drain with --max-conns {max_conns} closes the live connection",
                  closed >= 1, text.splitlines()[-1] if text else "no output")
            lines = [ln for ln in open(log.name, encoding="utf-8", errors="replace")
                     if "/f" in ln]
            check(f"access log kept with --max-conns {max_conns}", len(lines) == 1,
                  f"{len(lines)} lines")
        finally:
            if sock is not None:
                sock.close()
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            os.unlink(log.name)


def test_descriptor_exhaustion_backs_off():
    """BUG-14: on EMFILE the worker used to nanosleep(100ms) inside the event
    loop, stalling every other connection. It now parks the listener on the
    timer wheel; the process must stay alive and serve again once descriptors
    free up."""
    port = PORT + 4
    soft, hard = 64, 64
    def preexec():
        import resource
        resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))

    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "--root", ROOT, "--max-conns", "512"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, preexec_fn=preexec)
    socks = []
    try:
        if not wait_ready(proc, port):
            check("descriptor exhaustion: server started", False)
            return
        for _ in range(80):  # more than the limit: the accept queue backs up
            try:
                socks.append(socket.create_connection(("127.0.0.1", port), timeout=2))
            except OSError:
                break
        for s in socks:
            try:
                s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
                s.settimeout(3)
                s.recv(65536)
            except OSError:
                pass
            s.close()
        socks = []
        time.sleep(1.0)
        check("server survived descriptor exhaustion", proc.poll() is None)
        # After everything closed, the retry timer must bring the listener back.
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        s.settimeout(5)
        data = b""
        try:
            while True:
                c = s.recv(65536)
                if not c:
                    break
                data += c
        except socket.timeout:
            pass
        s.close()
        check("listener recovered after the backoff", data.startswith(b"HTTP/1.1 200"),
              repr(data[:40]))
    finally:
        for s in socks:
            s.close()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                _, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                err = b""
        else:
            err = proc.stderr.read() if proc.stderr else b""
        check("descriptor exhaustion was logged", b"accept4" in err,
              "no accept4 error line")


def main():
    print("regression: timing wheel (BUG-01)")
    test_idle_cpu_after_partial_request()
    print("regression: rejected connections (BUG-05)")
    test_rejected_connection_closes_fd()
    print("regression: shutdown drain (BUG-07)")
    test_shutdown_drain_reaches_every_connection()
    print("regression: descriptor exhaustion (BUG-14)")
    test_descriptor_exhaustion_backs_off()

    print()
    if failures:
        print(f"regression test: FAILED ({len(failures)} checks)")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("regression test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())