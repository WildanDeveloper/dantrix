#!/usr/bin/env python3
"""Integration smoke test for Dantrix v0.1 (TCP echo core).

Runs the built binary, exercises the echo path with sequential and concurrent
clients, verifies graceful shutdown on SIGTERM, and fails on any non-zero exit.
Dependency free: uses only the standard library.

Usage: python3 tests/smoke_test.py [path-to-binary]
"""

import os
import signal
import socket
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/dantrix-asan"
PORT = 18091
failures = []


def check(name, cond):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}")
    if not cond:
        failures.append(name)


def recv_all(sock, expected, timeout=5.0):
    sock.settimeout(timeout)
    data = b""
    deadline = time.time() + timeout
    while len(data) < expected and time.time() < deadline:
        chunk = sock.recv(expected - len(data))
        if not chunk:
            break
        data += chunk
    return data


def echo_roundtrip(payload, label):
    with socket.create_connection(("127.0.0.1", PORT), timeout=5) as s:
        s.sendall(payload)
        got = recv_all(s, len(payload))
        check(f"{label}: echo roundtrip ({len(payload)} bytes)", got == payload)


def main():
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "--log-level", "debug"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

    # Wait for the listener to come up.
    ready = False
    for _ in range(50):
        if proc.poll() is not None:
            out, err = proc.communicate()
            print(err.decode())
            print("server exited during startup")
            return 1
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                ready = True
                break
        except OSError:
            time.sleep(0.1)
    if not ready:
        proc.kill()
        print("server never accepted a connection")
        return 1
    print("server started")

    echo_roundtrip(b"hello\n", "small")
    echo_roundtrip(b"x" * 100_000, "100 KiB (larger than one read buffer)")
    echo_roundtrip(b"y" * 300_000, "300 KiB (exercises backpressure)")
    echo_roundtrip(b"", "empty write is legal")

    # Concurrent clients must not interfere with each other.
    socks = []
    ok = True
    try:
        for i in range(50):
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            socks.append(s)
        for i, s in enumerate(socks):
            s.sendall(f"client-{i}-payload".encode())
        for i, s in enumerate(socks):
            got = recv_all(s, len(f"client-{i}-payload"))
            ok = ok and got == f"client-{i}-payload".encode()
        check("50 concurrent connections echo independently", ok)
    finally:
        for s in socks:
            s.close()

    # A client that disconnects mid-stream must not wedge the server.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(b"partial")
    s.shutdown(socket.SHUT_WR)
    s.close()
    echo_roundtrip(b"after-rude-client", "server survives a rude disconnect")

    # Half-close: send data then FIN, the server should still echo.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(b"half-close\n")
    s.shutdown(socket.SHUT_WR)
    got = recv_all(s, len(b"half-close\n"))
    check("half-close echo before FIN handling", got == b"half-close\n")
    s.close()

    # Graceful shutdown: SIGTERM, exit code 0, clean logs.
    proc.send_signal(signal.SIGTERM)
    out, err = proc.communicate(timeout=10)
    check("SIGTERM -> exit code 0", proc.returncode == 0)
    text = err.decode()
    check("shutdown logged", "shutdown complete" in text)
    check("no sanitizer errors", "ERROR:" not in text and "runtime error" not in text)
    check("no assertion failures", "Assertion" not in text)
    if proc.returncode != 0 or "ERROR:" in text:
        print("--- stderr ---")
        print(text[-4000:])

    print("smoke test:", "FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
