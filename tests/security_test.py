#!/usr/bin/env python3
"""Security claim verification (v0.8.3 review).

Each test attacks the running server the way an untrusted client would and
asserts the documented mitigation actually holds. A claim that cannot be
verified empirically is reported as UNVERIFIED rather than assumed true.

Usage: python3 tests/security_test.py [path-to-binary]
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
PORT = int(os.environ.get("DANTRIX_TEST_PORT", "18101")) + 90
failures = []
unverified = []
ROOT = tempfile.mkdtemp(prefix="dantrix-sec-")
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def note_unverified(name, why):
    print(f"  [skip] {name}  ({why})")
    unverified.append(name)


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


def raw(port, payload, read_bytes=8192, timeout=3.0, split_after=None, split_delay=0.3):
    """Send raw bytes, optionally in two parts, and return everything received."""
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        if split_after is not None:
            s.sendall(payload[:split_after])
            time.sleep(split_delay)
            s.sendall(payload[split_after:])
        else:
            s.sendall(payload)
        s.settimeout(timeout)
        data = b""
        try:
            while True:
                chunk = s.recv(read_bytes)
                if not chunk:
                    break
                data += chunk
        except OSError:
            pass
        return data
    finally:
        s.close()


def build_root():
    """A document root with the files a traversal attack would want."""
    os.makedirs(os.path.join(ROOT, "sub"), exist_ok=True)
    with open(os.path.join(ROOT, "index.html"), "w", encoding="ascii") as f:
        f.write("<h1>ok</h1>")
    with open(os.path.join(ROOT, "sub", "child.txt"), "w", encoding="ascii") as f:
        f.write("child")
    # A secret OUTSIDE the root, one level up.
    outside = os.path.join(os.path.dirname(ROOT), "dantrix-secret.txt")
    with open(outside, "w", encoding="ascii") as f:
        f.write("TOP-SECRET-DO-NOT-SERVE")
    # A symlink inside the root pointing outside it.
    link = os.path.join(ROOT, "escape.txt")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(outside, link)
    # A hidden file.
    with open(os.path.join(ROOT, ".env"), "w", encoding="ascii") as f:
        f.write("SECRET=hunter2")
    os.makedirs(os.path.join(ROOT, ".git"), exist_ok=True)
    with open(os.path.join(ROOT, ".git", "config"), "w", encoding="ascii") as f:
        f.write("[core]")
    return outside


def start(args=None, port=None, root=None):
    port = port or PORT
    root = root or ROOT
    cmd = [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "1",
           "--access-log", "off", "--error-log", os.path.join(root, "err.log")]
    if args:
        cmd += args
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), port


def test_path_traversal(secret_path):
    """Claim: ../, encoded forms and NUL cannot escape the document root."""
    print("security: path traversal")
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            check("traversal: server started", False)
            return
        secret = b"TOP-SECRET-DO-NOT-SERVE"
        # Every form of "go up": raw, percent-encoded (once and twice), backslash,
        # dot-segments, and the NUL truncation trick.
        attacks = [
            b"/../dantrix-secret.txt",
            b"/sub/../../dantrix-secret.txt",
            b"/%2e%2e/dantrix-secret.txt",
            b"/%2e%2e%2f/dantrix-secret.txt",
            b"/..%2fdantrix-secret.txt",
            b"/%252e%252e/dantrix-secret.txt",
            b"/sub/..%5c..%5cdantrix-secret.txt",
            b"/./../dantrix-secret.txt",
            b"/....//dantrix-secret.txt",
            b"/%00/../dantrix-secret.txt",
            b"/index.html%00.png",
            b"/sub/./../../dantrix-secret.txt",
        ]
        leaked = []
        for a in attacks:
            data = raw(port, b"GET " + a + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            if secret in data:
                leaked.append(a.decode("latin1"))
        check("traversal: no attack leaked the file outside the root", not leaked,
              f"leaked via {leaked}")

        # The symlink out of the root must not be followed (follow_symlinks=off).
        data = raw(port, b"GET /escape.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        check("traversal: symlink out of the root is refused", secret not in data,
              data[:60].decode("latin1", "replace"))

        # A legitimate file must still work, so the refusals are not blanket.
        data = raw(port, b"GET /sub/child.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        check("traversal: a legitimate file is still served", b"child" in data)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_add_header_cannot_split_a_response():
    """Claim: a configured response header cannot inject another header.

    `add_header` is written into the response verbatim, so a CR or LF in the
    configuration would split it -- the same attack the request parser refuses on
    the input side. The configuration must reject that at start-up, because a
    value that reaches the renderer has already been trusted.
    """
    print("security: add_header validation")

    work = tempfile.mkdtemp(prefix="dantrix-addheader-")
    root = os.path.join(work, "root")
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "f"), "w", encoding="ascii") as f:
        f.write("x")

    # A CRLF in the value, written as real bytes.
    bad = os.path.join(work, "crlf.conf")
    with open(bad, "wb") as f:
        f.write(b"root = " + root.encode() + b"\n")
        f.write(b"add_header = X-A: b\r\nX-Forged: yes\n")
    proc = subprocess.run([BIN, "--test-config", "-c", bad], capture_output=True, text=True,
                          timeout=30)
    check("add_header: a CRLF in the value is refused at start-up", proc.returncode != 0,
          f"rc={proc.returncode}")

    # A framing header must be refused: it decides how the message is delimited.
    framing = os.path.join(work, "framing.conf")
    with open(framing, "w", encoding="ascii") as f:
        f.write(f"root = {root}\nadd_header = Content-Length: 999\n")
    proc = subprocess.run([BIN, "--test-config", "-c", framing], capture_output=True, text=True,
                          timeout=30)
    check("add_header: setting Content-Length is refused", proc.returncode != 0,
          f"rc={proc.returncode}")

    # A realistic security-header set must be accepted and must reach the client.
    good = os.path.join(work, "good.conf")
    with open(good, "w", encoding="ascii") as f:
        f.write(f"root = {root}\n")
        f.write("add_header = X-Frame-Options: DENY\n")
        f.write("add_header = Referrer-Policy: no-referrer\n")
        f.write("add_header = Permissions-Policy: geolocation=()\n")
    port = PORT + 30
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-c", good, "-w", "1",
         "--access-log", "off", "--error-log", os.path.join(work, "err.log")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("add_header: server started with a valid set", False)
            return
        for path in (b"/f", b"/missing"):
            data = raw(port, b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n"
                             b"Connection: close\r\n\r\n")
            head = data.split(b"\r\n\r\n", 1)[0]
            for name in (b"X-Frame-Options: DENY", b"Referrer-Policy: no-referrer",
                         b"Permissions-Policy: geolocation=()"):
                check(f"add_header: {name.decode()} present exactly once on {path.decode()}",
                      head.count(name) == 1, f"count={head.count(name)}")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    shutil.rmtree(work, ignore_errors=True)


def test_symlink_policy():
    """Claim: follow_symlinks=false refuses every link that leaves the root, and
    follow_symlinks=true still refuses the escaping ones while allowing the
    links that stay inside.

    A blanket refusal would pass a naive test, so both settings are exercised
    with three shapes at once: an absolute link out, a relative link out, and a
    link that stays inside.
    """
    print("security: symlink policy")

    work = tempfile.mkdtemp(prefix="dantrix-symlink-")
    root = os.path.join(work, "root")
    os.makedirs(os.path.join(root, "sub"), exist_ok=True)
    secret = os.path.join(work, "secret.txt")
    with open(secret, "w", encoding="ascii") as f:
        f.write("SYMLINK-SECRET")
    with open(os.path.join(root, "sub", "inner.txt"), "w", encoding="ascii") as f:
        f.write("inner")
    os.symlink(secret, os.path.join(root, "abs_out"))
    os.symlink("../secret.txt", os.path.join(root, "rel_out"))
    os.symlink("sub/inner.txt", os.path.join(root, "stays_in"))

    for follow in ("false", "true"):
        port = PORT + (20 if follow == "false" else 21)
        proc = subprocess.Popen(
            [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "1",
             "--access-log", "off", "--error-log", "/dev/null",
             "-o", f"follow_symlinks={follow}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if not wait_ready(proc, port):
                check(f"symlink(follow={follow}): server started", False)
                continue
            for path in (b"/abs_out", b"/rel_out"):
                data = raw(port, b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n"
                                 b"Connection: close\r\n\r\n")
                check(f"symlink(follow={follow}): {path.decode()} does not escape",
                      b"SYMLINK-SECRET" not in data, data[:40].decode("latin1", "replace"))
            data = raw(port, b"GET /stays_in HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            if follow == "true":
                check("symlink(follow=true): a link that stays inside is served",
                      b"inner" in data, data[:40].decode("latin1", "replace"))
            else:
                check("symlink(follow=false): a link is refused even when it stays inside",
                      b"inner" not in data, data[:40].decode("latin1", "replace"))
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    shutil.rmtree(work, ignore_errors=True)


def test_hardlink_is_a_root_entry():
    """A hard link is not a bypass, and the documentation must say so.

    A hard link created inside the document root IS an entry of that root: the
    kernel cannot distinguish it from a regular file, and every server serves it.
    This test records the behaviour explicitly so the SECURITY.md note about it
    cannot drift, and so a future reader does not mistake it for a defect.
    """
    print("security: hard link is a root entry (documented, not a bypass)")

    work = tempfile.mkdtemp(prefix="dantrix-hardlink-")
    root = os.path.join(work, "root")
    os.makedirs(root, exist_ok=True)
    outside = os.path.join(work, "outside.txt")
    with open(outside, "w", encoding="ascii") as f:
        f.write("HARD-LINK-CONTENT")
    try:
        os.link(outside, os.path.join(root, "hard"))
        linked = True
    except OSError:
        linked = False  # cross-device: cannot test here

    if not linked:
        note_unverified("hard link behaviour", "the filesystem does not allow hard links here")
        shutil.rmtree(work, ignore_errors=True)
        return

    port = PORT + 22
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "1",
         "--access-log", "off", "--error-log", "/dev/null"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("hardlink: server started", False)
            return
        data = raw(port, b"GET /hard HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        check("hardlink: served, because it is a real entry of the root",
              b"HARD-LINK-CONTENT" in data, data[:40].decode("latin1", "replace"))
        check("hardlink: the document root note covers this case",
              "hard link" in open(os.path.join(SRC, "SECURITY.md"), encoding="utf-8").read())
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(work, ignore_errors=True)


def test_hidden_files():
    """Claim: dot segments are refused unless hidden_files is on."""
    print("security: hidden files")
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            check("hidden: server started", False)
            return
        for path in (b"/.env", b"/.git/config", b"/sub/.hidden"):
            data = raw(port, b"GET " + path + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            check(f"hidden: {path.decode()} is not served",
                  b"SECRET" not in data and b"[core]" not in data, data[:40].decode("latin1", "replace"))
        # .well-known is the documented exception.
        os.makedirs(os.path.join(ROOT, ".well-known"), exist_ok=True)
        with open(os.path.join(ROOT, ".well-known", "test"), "w", encoding="ascii") as f:
            f.write("wk")
        data = raw(port, b"GET /.well-known/test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        check("hidden: .well-known is still reachable (documented exception)", b"wk" in data)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_request_smuggling():
    """Claim: conflicting framing is rejected, not guessed."""
    print("security: request smuggling")
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            check("smuggling: server started", False)
            return
        cases = [
            ("duplicate Content-Length",
             b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
             b"Content-Length: 6\r\n\r\nhello"),
            ("conflicting Content-Length values",
             b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
             b"Content-Length: 10\r\n\r\nhello"),
            ("Transfer-Encoding present",
             b"POST /index.html HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
            ("CL plus TE",
             b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
             b"Transfer-Encoding: chunked\r\n\r\nhello"),
            ("bare LF line endings",
             b"GET /index.html HTTP/1.1\nHost: x\n\n"),
            ("space before colon",
             b"GET /index.html HTTP/1.1\r\nHost : x\r\n\r\n"),
        ]
        for name, payload in cases:
            data = raw(port, payload)
            first = data.split(b"\r\n", 1)[0] if data else b"<no response>"
            rejected = first.startswith(b"HTTP/1.1 4") or first.startswith(b"HTTP/1.1 5")
            check(f"smuggling: {name} is rejected", rejected, first.decode("latin1", "replace"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_response_splitting():
    """Claim: client input never reaches response headers (no CRLF injection)."""
    print("security: response splitting")
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            check("splitting: server started", False)
            return
        # A path whose decoded form contains CRLF: if it were echoed into a
        # Location header the response would split.
        attacks = [
            b"/%0d%0aX-Injected:%20yes",
            b"/a%0d%0a%0d%0aHTTP/1.1%20200%20OK",
            b"/%0aX-Injected:%20yes",
        ]
        for a in attacks:
            data = raw(port, b"GET " + a + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            head = data.split(b"\r\n\r\n", 1)[0]
            check(f"splitting: {a.decode('latin1')} does not inject a header",
                  b"X-Injected" not in head, head[:80].decode("latin1", "replace"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_open_redirect():
    """Claim: a redirect Location always starts with a single slash."""
    print("security: open redirect")
    proc, port = start(["--directory-listing", "off"])
    try:
        if not wait_ready(proc, port):
            check("redirect: server started", False)
            return
        # A directory reached without a trailing slash is redirected. The
        # attack is a path that would build "//evil.com/".
        for target in (b"//sub", b"/sub", b"/%2fsub", b"/sub/"):
            data = raw(port, b"GET " + target + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            m = re.search(rb"\r\nLocation: ([^\r\n]+)", data)
            if m:
                loc = m.group(1)
                check(f"redirect: Location for {target.decode()} starts with one slash",
                      loc.startswith(b"/") and not loc.startswith(b"//"), loc.decode("latin1"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_version_disclosure():
    """Claim: server_tokens=off hides the version."""
    print("security: information disclosure")
    proc, port = start(["--server-tokens", "off"])
    try:
        if not wait_ready(proc, port):
            check("disclosure: server started", False)
            return
        data = raw(port, b"GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        head = data.split(b"\r\n\r\n", 1)[0]
        check("disclosure: server_tokens=off removes the Server header",
              b"Server:" not in head, head[:120].decode("latin1", "replace"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    # With tokens on (the default) the version is present, which is documented.
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            return
        data = raw(port, b"GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        check("disclosure: the default advertises the version (documented)",
              b"Server: Dantrix/" in data)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_log_injection():
    """Claim: control characters in the request cannot forge a log line."""
    print("security: log injection")
    log = os.path.join(ROOT, "access-inject.log")
    if os.path.exists(log):
        os.unlink(log)
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT + 1), "-r", ROOT, "-w", "1",
         "--access-log", log, "--log-format", "combined",
         "--error-log", os.path.join(ROOT, "err2.log")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, PORT + 1):
            check("log injection: server started", False)
            return
        # A User-Agent carrying CRLF, and a path carrying a newline. Both must be
        # escaped so a single request produces exactly one log line.
        raw(PORT + 1,
            b"GET /index.html HTTP/1.1\r\nHost: x\r\n"
            b"User-Agent: evil\r\nX-Forged: yes\r\nConnection: close\r\n\r\n")
        raw(PORT + 1,
            b"GET /index.html%0aFORGED-LINE HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        time.sleep(0.5)
        text = open(log, encoding="utf-8", errors="replace").read()
        lines = [ln for ln in text.splitlines() if ln.strip()]
        check("log injection: every request is exactly one log line", len(lines) == 2,
              f"{len(lines)} lines: {text[:200]!r}")
        # The real property: no line was *forged*. A forged line would start with
        # attacker-controlled text instead of the client address, so every line
        # must begin with the peer IP. (The request path is logged percent-encoded
        # and the request was refused with 400, so the text appears inside a
        # single well-formed line -- that is the mitigation working, not failing.)
        forged = [ln for ln in lines if not ln.startswith("127.0.0.1 ")]
        check("log injection: no forged line (every line starts with the client address)",
              not forged, f"{forged[:2]}")
        # And no line contains a raw CR/LF that split it in two.
        check("log injection: no raw newline reached the log file",
              "\nFORGED" not in text and "\rFORGED" not in text)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_resource_limits():
    """Claim: oversized requests and bodies are bounded, not buffered."""
    print("security: resource limits")
    proc, port = start(["--max-request-line", "1024", "--max-header-size", "2048",
                        "--max-headers", "10", "--max-body-size", "4096"])
    try:
        if not wait_ready(proc, port):
            check("limits: server started", False)
            return
        # Request line over the limit -> 414.
        data = raw(port, b"GET /" + b"a" * 4096 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
        check("limits: an oversized request line is refused",
              data.startswith(b"HTTP/1.1 414"), data[:40].decode("latin1", "replace"))
        # Too many headers -> 431.
        many = b"GET /index.html HTTP/1.1\r\nHost: x\r\n" + b"".join(
            b"X-H%d: v\r\n" % i for i in range(50)) + b"\r\n"
        data = raw(port, many)
        check("limits: too many headers are refused",
              data.startswith(b"HTTP/1.1 431"), data[:40].decode("latin1", "replace"))
        # A body larger than the limit must not be read forever.
        big = b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n"
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            s.sendall(big)
            s.sendall(b"A" * 20000)
            s.settimeout(3)
            data = s.recv(4096)
            check("limits: an oversized body is refused or closed, not buffered",
                  data.startswith(b"HTTP/1.1 4") or data == b"", data[:40].decode("latin1", "replace"))
        except OSError:
            check("limits: an oversized body is refused or closed, not buffered", True)
        finally:
            s.close()
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_no_privilege_escalation_after_drop():
    """Claim: after start-up the process cannot regain privileges.

    Verified by checking that the running server has no_new_privs set when
    seccomp is enabled, and that a dropped-privilege process reports the target
    uid. Requires root to test the drop itself; skipped otherwise.
    """
    print("security: privilege drop")
    if os.geteuid() != 0:
        note_unverified("privilege drop to a non-root user",
                        "needs root to start a server that binds and drops")
    else:
        proc, port = start(["--user", "nobody", "--seccomp", "on"], port=PORT + 2)
        try:
            if not wait_ready(proc, port):
                check("privilege: server started as root then dropped", False)
                return
            with open(f"/proc/{proc.pid}/status", encoding="ascii") as f:
                status = f.read()
            uid = re.search(r"^Uid:\s+(\d+)", status, re.M)
            nnp = re.search(r"^NoNewPrivs:\s+(\d+)", status, re.M)
            seccomp = re.search(r"^Seccomp:\s+(\d+)", status, re.M)
            check("privilege: the process runs as nobody after the drop",
                  uid is not None and uid.group(1) != "0", uid.group(0) if uid else "no Uid line")
            check("privilege: no_new_privs is set", nnp is not None and nnp.group(1) == "1",
                  nnp.group(0) if nnp else "no line")
            check("privilege: seccomp filter is active", seccomp is not None and seccomp.group(1) == "2",
                  seccomp.group(0) if seccomp else "no line")
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def test_parser_hardening_edges():
    """Claim: CRLF cannot reach a header, and integer input is validated.

    These are the two places where a bypass would be worst: a header built from
    client input (response splitting) and a length parsed from client input
    (integer overflow / smuggling).
    """
    print("security: parser hardening edges")
    proc, port = start()
    try:
        if not wait_ready(proc, port):
            check("edges: server started", False)
            return
        # A path that decodes to CRLF, requested so the directory-redirect path
        # would build a Location from it.
        for p in (b"/%0d%0aX-Inject:%20yes/", b"/a%0d%0aX-Inject:%20yes",
                  b"/%0aSet-Cookie:%20a=b/"):
            data = raw(port, b"GET " + p + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            head = data.split(b"\r\n\r\n", 1)[0]
            check(f"edges: {p.decode('latin1')} cannot inject a header",
                  b"X-Inject" not in head and b"Set-Cookie" not in head,
                  head[:80].decode("latin1", "replace"))
        # Content-Length values that would overflow or be reinterpreted.
        for cl in (b"99999999999999999999999999", b"18446744073709551616", b"-1",
                   b"0x10", b"+5", b"5 5"):
            data = raw(port, b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: " + cl +
                       b"\r\nConnection: close\r\n\r\n")
            first = data.split(b"\r\n", 1)[0] if data else b""
            check(f"edges: Content-Length {cl.decode()} is refused",
                  first.startswith(b"HTTP/1.1 400"), first.decode("latin1", "replace"))
        # Many overlapping ranges must not be expanded.
        ranges = b",".join(b"0-10" for _ in range(500))
        data = raw(port, b"GET /index.html HTTP/1.1\r\nHost: x\r\nRange: bytes=" + ranges +
                   b"\r\nConnection: close\r\n\r\n")
        check("edges: 500 ranges are not expanded into a multipart body",
              data.startswith(b"HTTP/1.1 200"), data[:40].decode("latin1", "replace"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_build_hardening():
    """Claim: the *release* binary is actually hardened.

    The Makefile flags are not evidence -- the binary is. This reads the ELF
    headers directly: PIE, full RELRO, a non-executable stack, the stack
    protector, and fortified libc calls.

    A sanitizer build is skipped: it is a diagnostic artifact, not something
    that ships, and `-fsanitize` replaces the allocator and the checks the
    hardening flags add. The claim is about the release build.
    """
    print("security: build hardening in the produced binary")

    if "asan" in BIN or "tsan" in BIN or "debug" in BIN:
        note_unverified("build hardening",
                        f"{BIN} is a sanitizer/debug build; the claim is about the release binary")
        return

    def run(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
        except (OSError, subprocess.SubprocessError):
            return ""

    elf = run(["readelf", "-hW", BIN])
    if not elf:
        note_unverified("build hardening", "readelf is not installed")
        return

    # PIE: the executable is a position-independent DYN.
    check("hardening: the binary is PIE", "DYN" in elf and "Position-Independent" in elf)

    # Non-executable stack and RELRO.
    segs = run(["readelf", "-lW", BIN])
    check("hardening: GNU_STACK is non-executable", "GNU_STACK" in segs and " RWE " not in segs)
    check("hardening: GNU_RELRO is present", "GNU_RELRO" in segs)

    # Full RELRO needs BIND_NOW (the PLT is resolved at load, then read-only).
    dyn = run(["readelf", "-dW", BIN])
    check("hardening: BIND_NOW is set (full RELRO)", "BIND_NOW" in dyn)

    # _FORTIFY_SOURCE turns libc calls into their checked variants.
    syms = run(["nm", "-D", BIN])
    if syms:
        fortified = len([ln for ln in syms.splitlines() if "_chk" in ln])
        check("hardening: fortified libc calls are linked in", fortified > 0,
              f"{fortified} *_chk symbols")
    else:
        note_unverified("fortified libc calls", "nm is not installed")

    # The stack protector is only linked when a function actually needs it.
    all_syms = run(["nm", BIN])
    if all_syms:
        check("hardening: the stack protector is active",
              "stack_chk" in all_syms)
    else:
        note_unverified("stack protector", "nm is not installed")


def test_slowloris_is_bounded():
    """A client that drips one byte at a time must not hold a connection open.

    The header timeout is documented as a deadline from the first byte, but it
    was refreshed on every read: a trickle reset it forever and the connection
    was never closed. This asserts the total time is capped.
    """
    print("security: slowloris is bounded in total time")

    port = PORT + 3
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(port), "-r", ROOT, "-w", "1",
         "--access-log", "off", "--error-log", os.path.join(ROOT, "err3.log"),
         "-o", "header_timeout=1s", "-o", "max_header_size=8192",
         "-o", "max_request_line=2048"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(proc, port):
            check("slowloris: server started", False)
            return
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        start = time.time()
        sent = 0
        closed_at = None
        try:
            while time.time() - start < 12:
                s.sendall(b"a")  # never a complete header block
                sent += 1
                time.sleep(0.4)
                s.settimeout(0.01)
                try:
                    if s.recv(100) == b"":
                        closed_at = time.time() - start
                        break
                except socket.timeout:
                    pass
                except OSError:
                    closed_at = time.time() - start
                    break
        except OSError:
            closed_at = time.time() - start
        s.close()
        elapsed = closed_at if closed_at is not None else time.time() - start
        check(f"slowloris: the trickle is closed after ~1s, not held ({elapsed:.1f}s)",
              elapsed < 6.0, f"still open after {elapsed:.1f}s ({sent} bytes sent)")

        # A genuinely slow client that completes inside the window must still work.
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /index.html HTTP/1.1\r\n")
        time.sleep(0.3)
        s.sendall(b"Host: x\r\nConnection: close\r\n\r\n")
        s.settimeout(3)
        data = s.recv(200)
        s.close()
        check("slowloris: a slow but complete request is still served",
              data.startswith(b"HTTP/1.1 200"), data[:40].decode("latin1", "replace"))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    secret = build_root()
    try:
        test_path_traversal(secret)
        test_add_header_cannot_split_a_response()
        test_symlink_policy()
        test_hardlink_is_a_root_entry()
        test_hidden_files()
        test_request_smuggling()
        test_response_splitting()
        test_open_redirect()
        test_version_disclosure()
        test_log_injection()
        test_resource_limits()
        test_parser_hardening_edges()
        test_build_hardening()
        test_slowloris_is_bounded()
        test_no_privilege_escalation_after_drop()
    finally:
        outside = os.path.join(os.path.dirname(ROOT), "dantrix-secret.txt")
        if os.path.exists(outside):
            os.unlink(outside)
        shutil.rmtree(ROOT, ignore_errors=True)

    print()
    if unverified:
        print(f"security test: {len(unverified)} claim(s) NOT verified (see [skip] above)")
    if failures:
        print(f"security test: FAILED ({len(failures)} checks)")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("security test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
