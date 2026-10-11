#!/usr/bin/env python3
"""Integration smoke test for Dantrix v0.4 (HTTP core, static files, keep-alive).

Runs the built binary against a temporary document root and exercises the
strict parser, the file serving path (index, redirect, listing, validators,
ranges), keep-alive, pipelining, request body draining, timeouts, the access
log, concurrency and graceful shutdown. Standard library only.

Usage: python3 tests/smoke_test.py [path-to-binary]
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/dantrix-asan"
PORT = 18091
failures = []

DOCROOT = tempfile.mkdtemp(prefix="dantrix-docroot-")
ACCESS_LOG = tempfile.NamedTemporaryFile(prefix="dantrix-access-", suffix=".log", delete=False)
ACCESS_LOG.close()


def check(name, cond):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}")
    if not cond:
        failures.append(name)


def setup_docroot():
    """Build a small tree: index, text file, binary file, dir without index."""
    os.makedirs(os.path.join(DOCROOT, "sub"), exist_ok=True)
    os.makedirs(os.path.join(DOCROOT, "noindex"), exist_ok=True)
    os.makedirs(os.path.join(DOCROOT, ".hidden"), exist_ok=True)
    with open(os.path.join(DOCROOT, "index.html"), "w") as f:
        f.write("<h1>Dantrix</h1>\n")
    with open(os.path.join(DOCROOT, "hello.txt"), "w") as f:
        f.write("Hello static world!\n")
    with open(os.path.join(DOCROOT, "big.bin"), "wb") as f:
        f.write(bytes(range(256)) * 12)  # 3072 bytes
    with open(os.path.join(DOCROOT, "sub", "page.html"), "w") as f:
        f.write("<p>sub</p>\n")
    with open(os.path.join(DOCROOT, "noindex", "readme.txt"), "w") as f:
        f.write("no index here\n")
    with open(os.path.join(DOCROOT, ".hidden", "secret.txt"), "w") as f:
        f.write("secret\n")
    with open(os.path.join(DOCROOT, ".env"), "w") as f:
        f.write("SECRET=1\n")


def read_response(sock, timeout=5.0, has_body=True):
    """Read exactly one HTTP response: headers plus Content-Length body.

    has_body=False is for HEAD, where the headers promise a length that no
    body bytes ever follow.
    """
    sock.settimeout(timeout)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return data
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
    if not has_body:
        return head + b"\r\n\r\n" + rest
    while len(rest) < length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return head + b"\r\n\r\n" + rest


def read_until_eof(sock, timeout=5.0):
    sock.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    return data


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=5)


def request(raw, timeout=5.0):
    with connect() as s:
        s.sendall(raw)
        return read_response(s, timeout)


def status_line(resp):
    return resp.split(b"\r\n", 1)[0].decode(errors="replace")


def body_of(resp):
    parts = resp.split(b"\r\n\r\n", 1)
    return parts[1] if len(parts) > 1 else b""


def header_of(resp, name):
    for line in resp.split(b"\r\n\r\n", 1)[0].split(b"\r\n"):
        if line.lower().startswith(name.lower() + b":"):
            return line.split(b":", 1)[1].strip()
    return None


def main():
    setup_docroot()
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "--log-level", "debug",
         "--root", DOCROOT, "--directory-listing", "on",
         "--header-timeout", "1s", "--keepalive-timeout", "2s", "--body-timeout", "1s",
         "--access-log", ACCESS_LOG.name, "--log-format", "combined"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

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

    # --- static files ------------------------------------------------------
    resp = request(b"GET / HTTP/1.1\r\nHost: smoke.test\r\nConnection: close\r\n\r\n")
    check("GET / serves index.html", resp.startswith(b"HTTP/1.1 200 OK"))
    check("index.html has text/html charset",
          header_of(resp, b"Content-Type") == b"text/html; charset=utf-8")
    check("index body", body_of(resp) == b"<h1>Dantrix</h1>\n")
    check("index has Last-Modified", header_of(resp, b"Last-Modified") is not None)
    check("index has ETag", header_of(resp, b"ETag") is not None)
    check("index has Accept-Ranges", header_of(resp, b"Accept-Ranges") == b"bytes")

    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    check("text/plain charset", header_of(resp, b"Content-Type") == b"text/plain; charset=utf-8")
    check("text body", body_of(resp) == b"Hello static world!\n")

    resp = request(b"HEAD /hello.txt HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    check("HEAD identical Content-Length", header_of(resp, b"Content-Length") == b"20")
    check("HEAD has no body", body_of(resp) == b"")

    with open(os.path.join(DOCROOT, "big.bin"), "rb") as f:
        expected = f.read()
    resp = request(b"GET /big.bin HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    check("sendfile body matches the file", body_of(resp) == expected)

    resp = request(b"GET /sub HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    check("directory without slash -> 301", resp.startswith(b"HTTP/1.1 301 "))
    check("301 Location", header_of(resp, b"Location") == b"/sub/")

    resp = request(b"GET /noindex/ HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    check("listing enabled -> 200 with entries", resp.startswith(b"HTTP/1.1 200 OK"))
    check("listing contains the file", b"readme.txt" in body_of(resp))

    # --- conditional requests and ranges ------------------------------------
    head = request(b"HEAD /hello.txt HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
    etag = header_of(head, b"ETag")
    last_mod = header_of(head, b"Last-Modified")

    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: " + etag +
                   b"\r\nConnection: close\r\n\r\n")
    check("If-None-Match match -> 304", resp.startswith(b"HTTP/1.1 304 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: *\r\n\r\n")
    check("If-None-Match: * -> 304", resp.startswith(b"HTTP/1.1 304 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: \"other\"\r\n\r\n")
    check("If-None-Match mismatch -> 200", resp.startswith(b"HTTP/1.1 200 OK"))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-Modified-Since: " +
                   last_mod + b"\r\nConnection: close\r\n\r\n")
    check("If-Modified-Since -> 304", resp.startswith(b"HTTP/1.1 304 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n"
                   b"If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n")
    check("stale If-Modified-Since -> 200", resp.startswith(b"HTTP/1.1 200 OK"))

    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=0-3\r\n\r\n")
    check("Range -> 206", resp.startswith(b"HTTP/1.1 206 "))
    check("206 Content-Range", header_of(resp, b"Content-Range") == b"bytes 0-3/20")
    check("206 body", body_of(resp) == b"Hell")
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=-5\r\n\r\n")
    check("suffix range serves the tail", body_of(resp) == b"rld!\n")
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=10-\r\n\r\n")
    check("open-ended range -> 206", resp.startswith(b"HTTP/1.1 206 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=100-200\r\n\r\n")
    check("out of range -> 416", resp.startswith(b"HTTP/1.1 416 "))
    check("416 Content-Range", header_of(resp, b"Content-Range") == b"bytes */20")
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=0-3,5-6\r\n\r\n")
    check("multi-range ignored -> full 200", resp.startswith(b"HTTP/1.1 200 OK"))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=abc\r\n\r\n")
    check("invalid range ignored -> 200", resp.startswith(b"HTTP/1.1 200 OK"))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=0-3\r\nIf-Range: " +
                   etag + b"\r\n\r\n")
    check("If-Range match keeps 206", resp.startswith(b"HTTP/1.1 206 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nRange: bytes=0-3\r\n"
                   b"If-Range: \"stale\"\r\n\r\n")
    check("If-Range mismatch -> full 200", resp.startswith(b"HTTP/1.1 200 OK"))

    # --- safety -------------------------------------------------------------
    for label, target in [
        ("path traversal", b"/../../etc/passwd"),
        ("encoded traversal", b"/%2e%2e%2f%2e%2e%2fetc%2fpasswd"),
        ("mixed encoded traversal", b"/..%2f..%2fetc%2fpasswd"),
    ]:
        resp = request(b"GET " + target + b" HTTP/1.1\r\nHost: s\r\n\r\n")
        check(label + " -> 400", resp.startswith(b"HTTP/1.1 400 "))
    resp = request(b"GET /.env HTTP/1.1\r\nHost: s\r\n\r\n")
    check("hidden file -> 404", resp.startswith(b"HTTP/1.1 404 "))
    resp = request(b"GET /missing.txt HTTP/1.1\r\nHost: s\r\n\r\n")
    check("missing file -> 404", resp.startswith(b"HTTP/1.1 404 "))

    # --- parser surface -----------------------------------------------------
    resp = request(b"GET /hello.txt HTTP/1.1\r\n\r\n")
    check("GET without Host -> 400", resp.startswith(b"HTTP/1.1 400 "))
    resp = request(b"POST /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
    check("POST -> 405 with Allow",
          resp.startswith(b"HTTP/1.1 405 ") and b"Allow: GET, HEAD, OPTIONS" in resp)
    resp = request(b"BREW / HTTP/1.1\r\nHost: s\r\n\r\n")
    check("unknown method -> 501", resp.startswith(b"HTTP/1.1 501 "))
    resp = request(b"OPTIONS * HTTP/1.1\r\nHost: s\r\n\r\n")
    check("OPTIONS * -> 200", resp.startswith(b"HTTP/1.1 200 OK"))
    resp = request(b"GET /hello.txt HTTP/2.0\r\nHost: s\r\n\r\n")
    check("HTTP/2.0 -> 505", resp.startswith(b"HTTP/1.1 505 "))

    # --- keep-alive ---------------------------------------------------------
    with connect() as s:
        statuses = []
        for _ in range(5):
            s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
            statuses.append(status_line(read_response(s)))
        check("5 requests on one connection", statuses == ["HTTP/1.1 200 OK"] * 5)

    with connect() as s:
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
        first = read_response(s)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: s\r\n\r\n")
        second = read_response(s)
        check("keep-alive across a file response",
              status_line(first) == "HTTP/1.1 200 OK" and
              status_line(second) == "HTTP/1.1 200 OK" and len(body_of(second)) == 3072)

    with connect() as s:
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
        resp = read_response(s)
        check("keep-alive response omits Connection: close",
              header_of(resp, b"Connection") is None)

    with connect() as s:
        s.sendall(b"HEAD /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
        read_response(s, has_body=False)
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
        resp = read_response(s)
        check("HEAD then GET on the same connection", body_of(resp) == b"Hello static world!\n")

    # HTTP/1.0 closes unless keep-alive is requested.
    resp = request(b"GET /hello.txt HTTP/1.0\r\nHost: s\r\n\r\n")
    check("HTTP/1.0 answers and closes", header_of(resp, b"Connection") == b"close")
    resp = request(b"GET /hello.txt HTTP/1.0\r\nHost: s\r\nConnection: keep-alive\r\n\r\n")
    check("HTTP/1.0 keep-alive answers", header_of(resp, b"Connection") == b"keep-alive")

    # Connection: close is honoured.
    with connect() as s:
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n")
        data = read_until_eof(s)
    check("Connection: close ends the response and the socket",
          status_line(data) == "HTTP/1.1 200 OK" and header_of(data, b"Connection") == b"close")

    # --- pipelining ---------------------------------------------------------
    with connect() as s:
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n" * 3)
        seen = [status_line(read_response(s)) for _ in range(3)]
    check("3 pipelined requests answered in order", seen == ["HTTP/1.1 200 OK"] * 3)

    # --- request bodies -----------------------------------------------------
    with connect() as s:
        s.sendall(b"POST /hello.txt HTTP/1.1\r\nHost: s\r\nContent-Length: 5\r\n\r\nhello")
        resp = read_response(s)
    check("POST with a body -> 405", resp.startswith(b"HTTP/1.1 405 "))

    with connect() as s:
        s.sendall(b"POST /hello.txt HTTP/1.1\r\nHost: s\r\nContent-Length: 5\r\n\r\nhe")
        time.sleep(0.15)
        s.sendall(b"llo")
        resp = read_response(s)
    check("body split across packets -> 405", resp.startswith(b"HTTP/1.1 405 "))

    with connect() as s:
        s.sendall(b"POST /hello.txt HTTP/1.1\r\nHost: s\r\nContent-Length: 50000000\r\n\r\n")
        resp = read_response(s)
    check("oversized body -> 413", resp.startswith(b"HTTP/1.1 413 "))

    # --- timeouts -----------------------------------------------------------
    with connect() as s:
        s.sendall(b"GET / HTTP/1.1\r\nHost: s\r\n")  # headers never complete
        start = time.time()
        data = read_until_eof(s, timeout=6)
    check("header timeout answers 408",
          data.startswith(b"HTTP/1.1 408 ") and not data.startswith(b"GET"))
    check("header timeout fires near the configured limit", 0.5 < time.time() - start < 5.0)

    # An idle connection that already served a request falls to keepalive_timeout.
    s = connect()
    s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\n\r\n")
    read_response(s)
    start = time.time()
    data = read_until_eof(s, timeout=8)
    idle_after = time.time() - start
    s.close()
    check("idle connection closes without a response", data == b"")
    check("keepalive timeout fires near the configured limit", 1.0 < idle_after < 6.0)

    # A connection that never sent anything hits the (shorter) header timeout.
    with connect() as s2:
        start = time.time()
        data = read_until_eof(s2, timeout=6)
        fresh_after = time.time() - start
    check("silent connection is closed without a response", data == b"")
    check("header timeout closes a silent connection", 0.5 < fresh_after < 5.0)

    # --- concurrency and rude clients ---------------------------------------
    socks = []
    ok = True
    try:
        for _ in range(50):
            socks.append(connect())
        for s in socks:
            s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: c\r\n\r\n")
        for s in socks:
            got = read_response(s)
            ok = ok and got.startswith(b"HTTP/1.1 200 OK") and body_of(got) == b"Hello static world!\n"
        check("50 concurrent file requests answered independently", ok)
    finally:
        for s in socks:
            s.close()

    # A client that disconnects mid-request must not wedge the server.
    s = connect()
    s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: partial")
    s.shutdown(socket.SHUT_WR)
    s.close()
    check("server survives a rude disconnect",
          request(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n").startswith(b"HTTP/1.1 200 OK"))

    # A client that disconnects mid-download must not wedge the server either.
    s = connect()
    s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
    s.recv(16)
    s.close()
    check("server survives a mid-download disconnect",
          request(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n").startswith(b"HTTP/1.1 200 OK"))

    # --- graceful shutdown --------------------------------------------------
    idle = connect()  # keeps a connection open across the shutdown
    proc.send_signal(signal.SIGTERM)
    idle.settimeout(10)
    try:
        idle.recv(100)
    except OSError:
        pass
    idle.close()
    out, err = proc.communicate(timeout=15)
    check("SIGTERM -> exit code 0 with an open connection", proc.returncode == 0)
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
    got_lines = [line for line in lines if '"GET /hello.txt HTTP/1.1" 200' in line]
    check("access log has one line per response", len(lines) >= 40)
    check("access log recorded repeated keep-alive requests", len(got_lines) >= 8)
    check("access log reports the served body size",
          any(line.endswith('200 20 "-" "-"') for line in got_lines))
    range_lines = [line for line in lines if '"GET /hello.txt HTTP/1.1" 206' in line]
    check("access log records partial responses", len(range_lines) >= 3)
    timeout_lines = [line for line in lines if " 408 " in line]
    check("access log records the 408", len(timeout_lines) >= 1)
    check("no unescaped quotes in log lines",
          all(line.count('"') % 2 == 0 for line in lines))

    os.unlink(ACCESS_LOG.name)
    shutil.rmtree(DOCROOT, ignore_errors=True)
    print("smoke test:", "FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())