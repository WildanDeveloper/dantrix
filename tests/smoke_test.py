#!/usr/bin/env python3
"""Integration smoke test for Dantrix v0.3 (HTTP core + static files).

Runs the built binary against a temporary document root, exercises the strict
parser, the file serving path (index, redirect, listing, validators, ranges),
the access log, concurrency and graceful shutdown. Standard library only.

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


def recv_all(sock, timeout=5.0):
    """Read until EOF (the server closes after every response until v0.4)."""
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

    # --- static files ------------------------------------------------------
    resp = request(b"GET / HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("GET / serves index.html", resp.startswith(b"HTTP/1.1 200 OK"))
    check("index.html has text/html charset",
          header_of(resp, b"Content-Type") == b"text/html; charset=utf-8")
    check("index body", body_of(resp) == b"<h1>Dantrix</h1>\n")
    check("index has Last-Modified", header_of(resp, b"Last-Modified") is not None)
    check("index has ETag", header_of(resp, b"ETag") is not None)
    check("index has Accept-Ranges", header_of(resp, b"Accept-Ranges") == b"bytes")
    check("no Content-Range on a full response", header_of(resp, b"Content-Range") is None)

    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("text/plain charset", header_of(resp, b"Content-Type") == b"text/plain; charset=utf-8")
    check("text body", body_of(resp) == b"Hello static world!\n")

    resp = request(b"HEAD /hello.txt HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("HEAD identical Content-Length", header_of(resp, b"Content-Length") == b"20")
    check("HEAD has no body", body_of(resp) == b"")

    with open(os.path.join(DOCROOT, "big.bin"), "rb") as f:
        expected = f.read()
    resp = request(b"GET /big.bin HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("sendfile body matches the file", body_of(resp) == expected)
    check("binary Content-Length",
          header_of(resp, b"Content-Length") == str(len(expected)).encode())

    resp = request(b"GET /sub HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("directory without slash -> 301", resp.startswith(b"HTTP/1.1 301 "))
    check("301 Location", header_of(resp, b"Location") == b"/sub/")

    resp = request(b"GET /noindex/ HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    check("listing enabled -> 200 with entries", resp.startswith(b"HTTP/1.1 200 OK"))
    check("listing contains the file", b"readme.txt" in body_of(resp))

    # --- conditional requests and ranges ------------------------------------
    head = request(b"HEAD /hello.txt HTTP/1.1\r\nHost: smoke.test\r\n\r\n")
    etag = header_of(head, b"ETag")
    last_mod = header_of(head, b"Last-Modified")

    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: " + etag + b"\r\n\r\n")
    check("If-None-Match match -> 304", resp.startswith(b"HTTP/1.1 304 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: *\r\n\r\n")
    check("If-None-Match: * -> 304", resp.startswith(b"HTTP/1.1 304 "))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-None-Match: \"other\"\r\n\r\n")
    check("If-None-Match mismatch -> 200", resp.startswith(b"HTTP/1.1 200 OK"))
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: s\r\nIf-Modified-Since: " +
                   last_mod + b"\r\n\r\n")
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
    resp = request(b"GET /../../etc/passwd HTTP/1.1\r\nHost: s\r\n\r\n")
    check("path traversal -> 400", resp.startswith(b"HTTP/1.1 400 "))
    resp = request(b"GET /%2e%2e%2f%2e%2e%2fetc%2fpasswd HTTP/1.1\r\nHost: s\r\n\r\n")
    check("encoded traversal -> 400", resp.startswith(b"HTTP/1.1 400 "))
    resp = request(b"GET /..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: s\r\n\r\n")
    check("mixed encoded traversal -> 400", resp.startswith(b"HTTP/1.1 400 "))
    resp = request(b"GET /.env HTTP/1.1\r\nHost: s\r\n\r\n")
    check("hidden file -> 404", resp.startswith(b"HTTP/1.1 404 "))
    resp = request(b"GET /.hidden/secret.txt HTTP/1.1\r\nHost: s\r\n\r\n")
    check("hidden directory -> 404", resp.startswith(b"HTTP/1.1 404 "))
    resp = request(b"GET /missing.txt HTTP/1.1\r\nHost: s\r\n\r\n")
    check("missing file -> 404", resp.startswith(b"HTTP/1.1 404 "))

    # --- parser surface (still strict) --------------------------------------
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

    # A request split into single bytes must still parse.
    with socket.create_connection(("127.0.0.1", PORT), timeout=5) as s:
        for byte in b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n":
            s.sendall(bytes([byte]))
            time.sleep(0.001)
        resp = recv_all(s)
    check("byte-by-byte delivery -> 200", resp.startswith(b"HTTP/1.1 200 OK"))

    # --- concurrency and rude clients ---------------------------------------
    socks = []
    ok = True
    try:
        for _ in range(50):
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            socks.append(s)
        for s in socks:
            s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: c\r\n\r\n")
        for s in socks:
            got = recv_all(s)
            ok = ok and got.startswith(b"HTTP/1.1 200 OK") and body_of(got) == b"Hello static world!\n"
        check("50 concurrent file requests answered independently", ok)
    finally:
        for s in socks:
            s.close()

    # A client that disconnects mid-request must not wedge the server.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: partial")
    s.shutdown(socket.SHUT_WR)
    s.close()
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n")
    check("server survives a rude disconnect", resp.startswith(b"HTTP/1.1 200 OK"))

    # A client that disconnects mid-download must not wedge the server either.
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
    s.recv(16)
    s.close()
    resp = request(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n")
    check("server survives a mid-download disconnect", resp.startswith(b"HTTP/1.1 200 OK"))

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
    got_lines = [line for line in lines if '"GET /hello.txt HTTP/1.1" 200' in line]
    check("access log has one line per response", len(lines) >= 30)
    check("access log recorded the file GET", len(got_lines) >= 5)
    check("access log reports the served body size",
          any(line.endswith('200 20 "-" "-"') for line in got_lines))
    range_lines = [line for line in lines if '"GET /hello.txt HTTP/1.1" 206' in line]
    # bytes=0-3, bytes=-5, bytes=10- and the If-Range 206 all land here.
    check("access log records partial responses", len(range_lines) >= 3)
    check("no unescaped quotes in log lines",
          all(line.count('"') % 2 == 0 for line in lines))

    os.unlink(ACCESS_LOG.name)
    shutil.rmtree(DOCROOT, ignore_errors=True)
    print("smoke test:", "FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())