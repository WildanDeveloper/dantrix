#!/usr/bin/env python3
"""Integration smoke test for Dantrix v0.2 (HTTP core).

Runs the built binary, exercises the strict parser end to end (valid methods,
every documented error status, split delivery), the access log, concurrency,
and graceful shutdown on SIGTERM. Dependency free: standard library only.

Usage: python3 tests/smoke_test.py [path-to-binary]
"""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/dantrix-asan"
PORT = 18091
ACCESS_LOG = tempfile.NamedTemporaryFile(prefix="dantrix-access-", suffix=".log", delete=False)
ACCESS_LOG.close()
failures = []


def check(name, cond):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}")
    if not cond:
        failures.append(name)


def recv_all(sock, timeout=5.0):
    """Read until EOF (the server closes after every response in v0.2)."""
    sock.settimeout(timeout)
    data = b""
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    return data


def request(raw, timeout=5.0):
    with socket.create_connection(("127.0.0.1", PORT), timeout=timeout) as s:
        s.sendall(raw)
        return recv_all(s, timeout)


def status_line(resp):
    return resp.split(b"\r\n", 1)[0].decode(errors="replace")


def main():
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "--log-level", "debug",
         "--access-log", ACCESS_LOG.name, "--log-format", "combined"],
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

    # --- valid requests ---------------------------------------------------
    resp = request(b"GET /hello?x=1 HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("GET -> 200", resp.startswith(b"HTTP/1.1 200 OK\r\n"))
    check("GET body present", resp.endswith(b"the HTTP core is alive.\n"))
    check("GET Content-Length matches body",
          b"Content-Length: %d\r\n" % len(resp.split(b"\r\n\r\n", 1)[1]) in resp)
    check("GET Connection: close", b"Connection: close\r\n" in resp)
    check("GET nosniff", b"X-Content-Type-Options: nosniff\r\n" in resp)
    check("GET has Date header", b"\r\nDate: " in resp)

    resp = request(b"HEAD / HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("HEAD -> 200", resp.startswith(b"HTTP/1.1 200 OK\r\n"))
    check("HEAD has Content-Length 39", b"Content-Length: 39\r\n" in resp)
    check("HEAD has no body", resp.endswith(b"\r\n\r\n"))

    resp = request(b"OPTIONS * HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("OPTIONS -> 200 with Allow",
          resp.startswith(b"HTTP/1.1 200 OK\r\n") and b"Allow: GET, HEAD, OPTIONS\r\n" in resp)

    resp = request(b"GET / HTTP/1.0\r\n\r\n")
    check("HTTP/1.0 without Host -> 200", resp.startswith(b"HTTP/1.1 200 OK\r\n"))

    # --- rejections --------------------------------------------------------
    cases = [
        (b"POST / HTTP/1.1\r\nHost: x\r\n\r\n", 405),
        (b"DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", 405),
        (b"BREW / HTTP/1.1\r\nHost: x\r\n\r\n", 501),
        (b"GET / HTTP/1.1\r\n\r\n", 400),                       # no Host
        (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400),  # duplicate Host
        (b"GET / HTTP/1.1\nHost: x\r\n\r\n", 400),               # bare LF
        (b"GET  / HTTP/1.1\r\n\r\n", 400),                       # double space
        (b"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400),            # space before colon
        (b"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
        (b"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
         b"Content-Length: 5\r\n\r\n", 400),
        (b"GET / HTTP/2.4\r\nHost: x\r\n\r\n", 505),
        (b"GET / HTTP/1.9\r\nHost: x\r\n\r\n", 505),
        (b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400),
    ]
    all_ok = True
    for raw, want in cases:
        resp = request(raw)
        got = status_line(resp)
        if not got.startswith(f"HTTP/1.1 {want} "):
            print(f"    expected {want}, got: {got}")
            all_ok = False
    check("13 strict-parser rejection cases (400/405/501/505)", all_ok)

    resp = request(b"POST / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("405 carries Allow header", b"Allow: GET, HEAD, OPTIONS\r\n" in resp)
    check("error pages do not reflect input",
          b"Host" not in resp.split(b"\r\n\r\n", 1)[1])

    # 414: request line past the limit.
    resp = request(b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
    check("oversized request line -> 414", resp.startswith(b"HTTP/1.1 414 "))

    # 431: headers past the default 16 KiB budget.
    resp = request(b"GET / HTTP/1.1\r\nHost: x\r\n" + b"X-Pad: " + b"a" * 17000 +
                   b"\r\n\r\n")
    check("oversized headers -> 431", resp.startswith(b"HTTP/1.1 431 "))

    # A request split into single bytes must still parse.
    with socket.create_connection(("127.0.0.1", PORT), timeout=5) as s:
        for byte in b"GET /split HTTP/1.1\r\nHost: x\r\n\r\n":
            s.sendall(bytes([byte]))
            time.sleep(0.001)
        resp = recv_all(s)
    check("byte-by-byte delivery -> 200", resp.startswith(b"HTTP/1.1 200 OK\r\n"))

    # --- concurrency and rude clients -------------------------------------
    socks = []
    ok = True
    try:
        for _ in range(50):
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            socks.append(s)
        for s in socks:
            s.sendall(b"GET / HTTP/1.1\r\nHost: c\r\n\r\n")
        for s in socks:
            got = recv_all(s)
            ok = ok and got.startswith(b"HTTP/1.1 200 OK\r\n")
        check("50 concurrent connections answered independently", ok)
    finally:
        for s in socks:
            s.close()

    # A client that disconnects mid-request must not wedge the server.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(b"GET / HTTP/1.1\r\nHost: partial")
    s.shutdown(socket.SHUT_WR)
    s.close()
    resp = request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("server survives a rude disconnect", resp.startswith(b"HTTP/1.1 200 OK\r\n"))

    # A client that sends nothing and closes must not wedge the server.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.close()
    resp = request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("server survives an empty connection", resp.startswith(b"HTTP/1.1 200 OK\r\n"))

    # --- graceful shutdown --------------------------------------------------
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

    # --- access log ---------------------------------------------------------
    with open(ACCESS_LOG.name, "r", errors="replace") as f:
        lines = [line.rstrip("\n") for line in f if line.strip()]
    os.unlink(ACCESS_LOG.name)
    check("access log has one line per response", len(lines) >= 20)
    get_lines = [line for line in lines if '"GET /hello?x=1 HTTP/1.1" 200' in line]
    check("access log recorded the GET", len(get_lines) == 1)
    check("access log has peer address", get_lines and get_lines[0].startswith("127.0.0.1 - - ["))
    check("access log combined format",
          get_lines and '"-"' in get_lines[0].rsplit('" 200', 1)[-1] or
          len(get_lines[0].split('"')) >= 5)
    code_lines = [line for line in lines if ' 400 ' in line or ' 505 ' in line]
    check("rejected requests are logged too", len(code_lines) >= 3)
    check("no unescaped quotes in log lines",
          all(line.count('"') % 2 == 0 for line in lines))

    print("smoke test:", "FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
