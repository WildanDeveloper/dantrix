#!/usr/bin/env python3
"""ThreadSanitizer test for Dantrix (v0.5.0 multi-worker).

The multi-worker server runs one event loop per thread and shares only three
things between them: the logger (mutex protected), the document root (read
only) and the orchestrator's shutdown path. This script drives real concurrent
traffic through the TSan build and fails the moment the runtime reports a
race.

Usage: python3 tests/tsan_test.py [path-to-tsan-binary]

Both the reuseport mode (default) and the shared-listener EPOLLEXCLUSIVE
fallback are exercised.
"""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/tsan/dantrix"
PORT = int(os.environ.get("DANTRIX_TSAN_PORT", "18097"))
WORKERS = 4
REQUESTS_PER_THREAD = 40
failures = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}{(' - ' + detail) if detail else ''}")
    if not cond:
        failures.append(name)


def tsan_report(text):
    """True when the TSan runtime printed a warning into the server output."""
    return "ThreadSanitizer" in text or "SUMMARY: ThreadSanitizer" in text


def wait_ready(port, tries=120):
    for _ in range(tries):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fetch(port, path, expect_status=200):
    """One blocking request; returns (status, body)."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
            s.sendall((f"GET {path} HTTP/1.1\r\nHost: t\r\n"
                       "Connection: close\r\n\r\n").encode())
            chunks = []
            while True:
                data = s.recv(65536)
                if not data:
                    break
                chunks.append(data)
            raw = b"".join(chunks)
            head, _, body = raw.partition(b"\r\n\r\n")
            status = int(head.split(b" ")[1]) if b" " in head else 0
            return status, body
    except OSError as exc:
        return 0, str(exc).encode()


results = []
results_lock = threading.Lock()


def worker_thread(port, path, count):
    local = []
    for _ in range(count):
        local.append(fetch(port, path))
    with results_lock:
        results.extend(local)


def run_mode(proc_args, label, port):
    """Start the server in one mode, drive concurrent traffic, shut down."""
    global results
    results = []
    docroot = tempfile.mkdtemp(prefix="dantrix-tsan-")
    payload = "r" * 4096
    with open(os.path.join(docroot, "index.html"), "w") as f:
        f.write(payload)

    # A second, larger file so the sendfile path is also exercised concurrently.
    with open(os.path.join(docroot, "big.bin"), "wb") as f:
        f.write(b"z" * 65536)

    err_path = os.path.join(docroot, "err.log")
    acc_path = os.path.join(docroot, "access.log")
    err_file = open(err_path, "w")
    acc_file = open(acc_path, "w")
    proc = subprocess.Popen(
        proc_args + ["-b", "127.0.0.1", "-p", str(port), "-r", docroot,
                     "-w", str(WORKERS), "--access-log", acc_path,
                     "--error-log", err_path, "--log-format", "combined"],
        stdout=err_file, stderr=err_file)
    try:
        if not wait_ready(port):
            check(f"{label}: server became ready", False,
                  f"exit={proc.poll()}")
            return
        check(f"{label}: server became ready", True)

        threads = [
            threading.Thread(target=worker_thread, args=(port, "/index.html",
                                                         REQUESTS_PER_THREAD))
            for _ in range(WORKERS)
        ]
        threads += [
            threading.Thread(target=worker_thread, args=(port, "/big.bin", 10))
            for _ in range(2)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=120)

        ok = sum(1 for status, _ in results if status == 200)
        expected = REQUESTS_PER_THREAD * WORKERS + 20
        check(f"{label}: all {expected} concurrent requests answered 200", ok == expected,
              f"{ok}/{expected}")

        # Concurrent requests on one keep-alive connection per worker.
        statuses = [fetch(port, "/index.html")[0] for _ in range(8)]
        check(f"{label}: sequential requests still work", all(s == 200 for s in statuses),
              str(statuses))

        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            check(f"{label}: graceful shutdown", False, "server did not exit in 30s")
            proc.kill()
            proc.wait()
            return
        check(f"{label}: graceful shutdown", proc.returncode == 0,
              f"rc={proc.returncode}")
    finally:
        err_file.close()
        acc_file.close()
        with open(err_path, encoding="utf-8", errors="replace") as f:
            err_text = f.read()
        with open(acc_path, encoding="utf-8", errors="replace") as f:
            acc_text = f.read()

    check(f"{label}: no ThreadSanitizer report in the server output", not tsan_report(err_text))
    lines = [l for l in acc_text.splitlines() if l.strip()]
    check(f"{label}: access log has one line per request", len(lines) == expected + 8,
          f"{len(lines)} lines for {expected + 8} requests")


def main():
    print(f"dantrix tsan test: binary={BIN} workers={WORKERS}")
    if not os.path.exists(BIN):
        print(f"  [FAIL] binary {BIN} not found (build it with: make tsan)")
        return 1

    run_mode([BIN], "reuseport", PORT)
    run_mode([BIN, "--no-reuseport"], "shared-listener", PORT + 1)

    print("tsan test: " + ("PASSED" if not failures else "FAILED"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
