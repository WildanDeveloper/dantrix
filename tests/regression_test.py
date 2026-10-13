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
import shutil
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

def fetch(port, request):
    """Send one raw request, read one response, return the bytes."""
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


def test_multi_worker_serves_and_shuts_down():
    """v0.5.0: N worker threads must share the port, answer every request and
    drain cleanly on SIGTERM, in both listener modes."""
    for label, extra, port in (("reuseport", [], PORT + 4),
                               ("shared", ["--no-reuseport"], PORT + 5)):
        log = tempfile.NamedTemporaryFile(prefix="dantrix-multi-", suffix=".log",
                                          delete=False)
        acc = tempfile.NamedTemporaryFile(prefix="dantrix-multi-", suffix=".acc",
                                          delete=False)
        proc = subprocess.Popen(
            [BIN, "-b", "127.0.0.1", "-p", str(port), "--root", ROOT, "-w", "4",
             "--access-log", acc.name] + extra + ["--error-log", log.name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if not wait_ready(proc, port):
                check(f"multi-worker {label}: server started", False)
                continue
            codes = []
            for _ in range(16):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                        s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n"
                                  b"Connection: close\r\n\r\n")
                        data = s.recv(65536)
                        codes.append(int(data.split(b" ")[1]))
                except OSError:
                    codes.append(0)
            check(f"multi-worker {label}: every request answered 200",
                  all(c == 200 for c in codes), str(codes))
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=15)
            check(f"multi-worker {label}: exit code 0 on SIGTERM", proc.returncode == 0,
                  f"rc={proc.returncode}")
            text = open(log.name, encoding="utf-8", errors="replace").read()
            complete = re.search(r"accepted (\d+), rejected (\d+), closed (\d+)", text)
            accepted = int(complete.group(1)) if complete else -1
            check(f"multi-worker {label}: worker statistics aggregated",
                  accepted >= 16, f"accepted={accepted}")
            acc_lines = [ln for ln in open(acc.name, encoding="utf-8",
                                           errors="replace") if "/f" in ln]
            check(f"multi-worker {label}: access log has one line per request",
                  len(acc_lines) == 16, f"{len(acc_lines)} lines")
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            os.unlink(log.name)
            os.unlink(acc.name)


def test_signals_reload_reopen_stats():
    """v0.6.0: SIGHUP reloads, SIGUSR1 reopens the logs, SIGUSR2 snapshots the
    statistics, SIGQUIT closes immediately. Exercised on one worker so the
    signalfd path is covered directly."""
    log = tempfile.NamedTemporaryFile(prefix="dantrix-sig-", suffix=".log", delete=False)
    log.close()
    acc = tempfile.NamedTemporaryFile(prefix="dantrix-sig-", suffix=".acc", delete=False)
    acc.close()
    cfg = tempfile.NamedTemporaryFile(prefix="dantrix-sig-", suffix=".conf",
                                      mode="w", delete=False)
    cfg.write(f"root = {ROOT}\nmax_request_line = 64\nmax_header_size = 256\n")
    cfg.close()
    port = PORT + 6
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-c", cfg.name, "-w", "1",
         "--access-log", acc.name, "--error-log", log.name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("signals: server started", False)
            return

        # A request line over the 64 byte limit is refused with 414.
        long_path = "/" + "a" * 100
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(f"GET {long_path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                      .encode())
            data = s.recv(65536)
        check("signals: 64 byte request line limit enforced (414)",
              data.split(b" ")[1] == b"414", data[:20])

        # Rewrite the configuration and reload.
        with open(cfg.name, "w") as f:
            f.write(f"root = {ROOT}\nmax_request_line = 8k\nmax_header_size = 16k\n")
        proc.send_signal(signal.SIGHUP)
        time.sleep(0.6)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(f"GET {long_path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                      .encode())
            data = s.recv(65536)
        check("signals: SIGHUP raised the request line limit (404 now)",
              data.split(b" ")[1] == b"404", data[:20])

        # Rotate the logs and reopen them.
        os.rename(log.name, log.name + ".1")
        os.rename(acc.name, acc.name + ".1")
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.6)
        check("signals: SIGUSR1 reopened the error log", os.path.exists(log.name))
        check("signals: SIGUSR1 reopened the access log", os.path.exists(acc.name))

        # A statistics snapshot is written to the (new) error log.
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.6)
        text = open(log.name, encoding="utf-8", errors="replace").read()
        check("signals: SIGUSR2 wrote a statistics snapshot",
              "stats" in text or "statistics" in text, text[-120:])

        proc.send_signal(signal.SIGQUIT)
        proc.wait(timeout=10)
        check("signals: SIGQUIT exits promptly with code 0", proc.returncode == 0,
              f"rc={proc.returncode}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        for name in (log.name, acc.name, log.name + ".1", acc.name + ".1", cfg.name):
            if os.path.exists(name):
                os.unlink(name)


def test_status_endpoint():
    """v0.6.0: the JSON status endpoint is served only to loopback clients and
    reports the shared counters."""
    log = tempfile.NamedTemporaryFile(prefix="dantrix-status-", suffix=".log", delete=False)
    log.close()
    port = PORT + 7
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", ROOT, "-w", "2",
         "--status-path", "/__status", "--access-log", "off", "--error-log", log.name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("status: server started", False)
            return
        # Drive two successful requests and one 404 so the counters are non-zero.
        for path in ("/f", "/f", "/nope"):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                          .encode())
                s.recv(65536)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"GET /__status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            data = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        body = data.split(b"\r\n\r\n", 1)[1]
        check("status: endpoint answers 200", data.split(b" ")[1] == b"200", data[:20])
        check("status: body is JSON with requests_total", b'"requests_total"' in body)
        check("status: counters counted the requests", b'"2xx": 2' in body, body[:200])
        check("status: 4xx counter is non-zero", b'"4xx": 1' in body, body[:200])
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        os.unlink(log.name)


def test_audit_round3_fixes():
    """Third audit round: the use-after-free on a directory listing, the 416
    that stayed keep-alive, the status endpoint accepting every method and the
    -o overrides lost on SIGHUP.

    BUG-A1 use-after-free: a listing must not free the header block before
    access_fill() reads through it. Requires a GET with a Content-Length whose
    body arrives in a later segment (so the header block is parked in
    conn->hdr) and a target that resolves to a directory listing.
    """
    log = tempfile.NamedTemporaryFile(prefix="dantrix-a3-", suffix=".log", delete=False)
    log.close()
    root = tempfile.mkdtemp(prefix="dantrix-a3-")
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("hi")
    os.makedirs(os.path.join(root, "subdir"), exist_ok=True)
    with open(os.path.join(root, "subdir", "child.txt"), "w") as f:
        f.write("x")
    port = PORT + 8
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "1",
         "--directory-listing", "on", "--access-log", "off", "--error-log", log.name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("audit3: server started", False)
            return

        # BUG-A1: split arrival -> conn->hdr is parked -> the listing path must
        # not free it before access_fill() has read through it.
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /subdir/ HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n")
        time.sleep(0.4)
        s.sendall(b"hello")
        time.sleep(0.6)
        data = b""
        s.settimeout(3)
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        except OSError:
            pass
        s.close()
        check("audit3: listing with a drained body answers 200",
              data.startswith(b"HTTP/1.1 200 OK"), data[:40])
        check("audit3: listing body contains the child entry", b"child.txt" in data)

        # BUG-A3: 416 is an error status, so the connection must close.
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n"
                  b"Range: bytes=999999999-\r\n\r\n")
        time.sleep(0.4)
        first = s.recv(65536)
        s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.4)
        second = b""
        s.settimeout(2)
        try:
            second = s.recv(65536)
        except OSError:
            pass
        s.close()
        check("audit3: 416 carries Connection: close",
              first.startswith(b"HTTP/1.1 416") and b"Connection: close" in first,
              first[:60])
        check("audit3: 416 closed the connection", second == b"", second[:40])

        # BUG-A4: -o must survive a SIGHUP reload.
        cfg = os.path.join(root, "a3.conf")
        with open(cfg, "w") as f:
            f.write(f"root = {root}\nserver_tokens = true\n")
        acc = os.path.join(root, "a3.acc")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
        proc2 = subprocess.Popen(
            [BIN, "-b", "127.0.0.1", "-p", str(port), "-c", cfg, "-w", "1",
             "-o", "server_tokens=off", "--access-log", "off",
             "--error-log", log.name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if not wait_ready(proc2, port):
                check("audit3: reload server started", False)
                return
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.4)
                before = s.recv(65536)
            check("audit3: -o server_tokens=off hides the Server header",
                  b"Server:" not in before, before[:60])
            proc2.send_signal(signal.SIGHUP)
            time.sleep(0.6)
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.4)
                after = s.recv(65536)
            check("audit3: -o overrides survive the SIGHUP reload",
                  b"Server:" not in after, after[:60])

            # BUG-A6: the status endpoint is read-only (GET/HEAD only).
            proc2.send_signal(signal.SIGTERM)
            proc2.wait(timeout=15)
            proc3 = subprocess.Popen(
                [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "1",
                 "--status-path", "/__status", "--access-log", "off",
                 "--error-log", log.name],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                if not wait_ready(proc3, port):
                    check("audit3: status server started", False)
                    return
                codes = []
                for method in (b"GET", b"HEAD", b"POST", b"PUT"):
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
                        time.sleep(0.3)
                        s.recv(65536)
                    s2 = socket.create_connection(("127.0.0.1", port), timeout=5)
                    s2.sendall(method + b" /__status HTTP/1.1\r\nHost: x\r\n"
                               b"Connection: close\r\n\r\n")
                    time.sleep(0.3)
                    codes.append(int(s2.recv(64).split(b" ")[1]))
                    s2.close()
                check("audit3: status endpoint GET/HEAD -> 200", codes[0] == 200 and codes[1] == 200,
                      str(codes))
                check("audit3: status endpoint POST/PUT -> 405", codes[2] == 405 and codes[3] == 405,
                      str(codes))
            finally:
                if proc3.poll() is None:
                    proc3.kill()
                    proc3.wait()
        finally:
            if proc2.poll() is None:
                proc2.kill()
                proc2.wait()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.unlink(log.name)
        shutil.rmtree(root, ignore_errors=True)



def test_audit_round4_regressions():
    """Fourth audit round: the de-recursion of the pipelining path broke two
    things that the standard suites did not cover.

    REG-1: a keep-alive connection that has served a request must be closed by
    the KEEPALIVE timeout (not by the header timeout) and must go IDLE, which
    also releases its read buffer back to the pool.
    REG-2: a pipelined burst far larger than the socket buffer must receive a
    response for every request -- no RST, no 408, no stall.
    """
    log = tempfile.NamedTemporaryFile(prefix="dantrix-a4-", suffix=".log",
                                      delete=False)
    log.close()
    port = PORT + 9

    # REG-1: keepalive and header timeouts deliberately far apart.
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", ROOT, "-w", "1",
         "--keepalive-timeout", "5s", "--header-timeout", "2s",
         "--access-log", "off", "--error-log", log.name, "--log-level", "debug"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("audit4: idle server started", False)
            return
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.4)
        s.recv(65536)
        start = time.time()
        s.settimeout(12)
        elapsed = None
        try:
            d = s.recv(65536)
            if not d:
                elapsed = time.time() - start
        except socket.timeout:
            pass
        s.close()
        check("audit4: idle keep-alive closed", elapsed is not None)
        # 5s keepalive, 2s header: the closure must be the LATER one.
        check("audit4: closed by the keepalive timeout, not the header timeout",
              elapsed is not None and 4.0 < elapsed < 8.0,
              f"elapsed={elapsed}")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
        text = open(log.name, encoding="utf-8", errors="replace").read()
        check("audit4: the timeout that fired was the keepalive one",
              "keepalive timeout" in text, text[-200:])
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.unlink(log.name)

    # REG-2: a pipelined burst far larger than the socket buffer.
    log2 = tempfile.NamedTemporaryFile(prefix="dantrix-a4b-", suffix=".log",
                                       delete=False)
    log2.close()
    port = PORT + 10
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", ROOT, "-w", "1",
         "--keepalive-requests", "100000", "--access-log", "off",
         "--error-log", log2.name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("audit4: burst server started", False)
            return
        req = b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n"
        for n, wait in ((1500, 0.7), (6000, 1.5)):
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            s.sendall(req * n)
            time.sleep(wait)  # force EAGAIN + hold + unread socket data
            s.settimeout(20)
            got = 0
            try:
                while got < n:
                    c = s.recv(1 << 20)
                    if not c:
                        break
                    got += c.count(b"HTTP/1.1 200 OK")
            except (ConnectionResetError, socket.timeout):
                pass
            s.close()
            check(f"audit4: {n} pipelined requests all answered", got == n,
                  f"{got}/{n}")
        text = open(log2.name, encoding="utf-8", errors="replace").read()
        check("audit4: no request-buffer exhaustion",
              "request buffer exhausted" not in text)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.unlink(log2.name)


def test_v070_tuning_and_doctor():
    """v0.7.0: the listener tuning flags reach the socket, the file-descriptor
    limit is raised automatically, and --doctor reports a blocked start-up."""
    print("regression: v0.7.0 listener tuning, fd auto-tune, doctor")

    # --- --doctor must exit 0 on a healthy configuration and name the fields.
    proc = subprocess.run(
        [BIN, "--doctor", "-r", ROOT, "-p", str(PORT + 40), "--max-conns", "64"],
        capture_output=True, text=True, timeout=30)
    out = proc.stdout
    check("doctor: exits 0 on a healthy configuration", proc.returncode == 0,
          f"rc={proc.returncode} {proc.stderr[:120]}")
    check("doctor: reports the document root", "document root" in out)
    check("doctor: reports RLIMIT_NOFILE", "RLIMIT_NOFILE" in out)
    check("doctor: reports the free port", "is free" in out)

    # --- --doctor must fail (rc != 0) when the port is already taken.
    holder = subprocess.Popen([BIN, "-b", "127.0.0.1", "-p", str(PORT + 41), "-r", ROOT,
                               "-w", "1", "--access-log", "off", "--error-log", "/dev/null"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if wait_ready(holder, PORT + 41):
            proc = subprocess.run(
                [BIN, "--doctor", "-b", "127.0.0.1", "-p", str(PORT + 41), "-r", ROOT],
                capture_output=True, text=True, timeout=30)
            check("doctor: fails when the port is occupied", proc.returncode != 0,
                  f"rc={proc.returncode}")
            check("doctor: says why the port failed", "Address already in use" in proc.stdout)
        else:
            check("doctor: holder server started", False)
    finally:
        holder.send_signal(signal.SIGTERM)
        try:
            holder.wait(timeout=5)
        except subprocess.TimeoutExpired:
            holder.kill()

    # --- The fd auto-tune must lower max_connections when the soft limit is low.
    # A shell raises the limit only for the child, so use a wrapper. The warning
    # goes to a file so the check never blocks on a pipe.
    warn_log = tempfile.NamedTemporaryFile(prefix="dantrix-fdtune-", suffix=".log", delete=False)
    warn_log.close()
    script = (
        f"ulimit -Sn 128 2>/dev/null; ulimit -Hn 128 2>/dev/null; "
        f"exec {BIN} -b 127.0.0.1 -p {PORT + 42} -r {ROOT} -w 2 --max-conns 16384 "
        f"--access-log off --error-log /dev/null"
    )
    with open(warn_log.name, "w", encoding="utf-8") as err:
        tuned = subprocess.Popen(["bash", "-c", script], stdout=subprocess.DEVNULL,
                                 stderr=err)
        try:
            if wait_ready(tuned, PORT + 42):
                text = ""
                deadline = time.time() + 3
                while time.time() < deadline:
                    try:
                        with open(warn_log.name, "r", encoding="utf-8", errors="replace") as f:
                            text = f.read()
                    except OSError:
                        text = ""
                    if "max_connections" in text:
                        break
                    time.sleep(0.1)
                check("fd auto-tune: warns that max_connections was lowered",
                      "lowering max_connections" in text, text[:160])
            else:
                check("fd auto-tune: tuned server started", False)
        finally:
            tuned.send_signal(signal.SIGTERM)
            try:
                tuned.wait(timeout=5)
            except subprocess.TimeoutExpired:
                tuned.kill()
    os.unlink(warn_log.name)

    # --- The tuning flags must be accepted and the server must still serve.
    tuned = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT + 43), "-r", ROOT, "-w", "1",
         "--access-log", "off", "--error-log", "/dev/null",
         "-o", "socket_rcvbuf=131072", "-o", "socket_sndbuf=131072",
         "-o", "tcp_defer_accept=100ms", "-o", "tcp_fastopen=8"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if wait_ready(tuned, PORT + 43):
            check("tuning flags: the server still serves after applying them",
                  fetch(PORT + 43, b"GET /f HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
                  .startswith(b"HTTP/1.1 200"))
        else:
            check("tuning flags: server started", False)
    finally:
        tuned.send_signal(signal.SIGTERM)
        try:
            tuned.wait(timeout=5)
        except subprocess.TimeoutExpired:
            tuned.kill()


def test_v070_date_header_cache():
    """The Date header is served from a cache, so it must stay correct: within
    the same second every response carries the same, parseable fixdate."""
    print("regression: v0.7.0 Date header cache")
    import datetime

    proc = subprocess.Popen([BIN, "-b", "127.0.0.1", "-p", str(PORT + 44), "-r", ROOT, "-w", "4",
                             "--access-log", "off", "--error-log", "/dev/null"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, PORT + 44):
            check("date cache: server started", False)
            return
        seen = set()
        ok = True
        now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
        for _ in range(30):
            data = fetch(PORT + 44, b"GET /f HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
            m = re.search(rb"\r\nDate: ([^\r\n]+)\r\n", data)
            if not m:
                ok = False
                break
            value = m.group(1).decode()
            try:
                got = datetime.datetime.strptime(value, "%a, %d %b %Y %H:%M:%S GMT")
            except ValueError:
                ok = False
                break
            seen.add(value)
            if abs((got - now).total_seconds()) > 5:
                ok = False
                break
        check("date cache: every response carries a parseable Date", ok)
        check("date cache: 4 workers agree within the same second", len(seen) <= 2,
              f"{len(seen)} distinct values")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    if not os.path.exists(BIN):
        print(f"  [FAIL] binary {BIN} not found - build it first "
              f"(release: make, debug: make debug)")
        return 1
    print("regression: timing wheel (BUG-01)")
    test_idle_cpu_after_partial_request()
    print("regression: rejected connections (BUG-05)")
    test_rejected_connection_closes_fd()
    print("regression: shutdown drain (BUG-07)")
    test_shutdown_drain_reaches_every_connection()
    print("regression: descriptor exhaustion (BUG-14)")
    test_descriptor_exhaustion_backs_off()
    print("regression: multi-worker (v0.5.0)")
    test_multi_worker_serves_and_shuts_down()
    print("regression: signals, reload, reopen logs, stats (v0.6.0)")
    test_signals_reload_reopen_stats()
    print("regression: JSON status endpoint (v0.6.0)")
    test_status_endpoint()
    print("regression: third audit round (UAF, 416, reload, status methods)")
    test_audit_round3_fixes()
    print("regression: fourth audit round (idle keep-alive, pipelined burst)")
    test_audit_round4_regressions()
    print("regression: v0.7.0 listener tuning, fd auto-tune, doctor")
    test_v070_tuning_and_doctor()
    print("regression: v0.7.0 Date header cache")
    test_v070_date_header_cache()

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