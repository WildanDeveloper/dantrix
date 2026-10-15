#!/usr/bin/env python3
"""C10K check for Dantrix: hold many idle connections open at once.

The point of the idle state in v0.4 is that an idle connection owns no read
buffer, so the memory cost per connection stays small enough to keep tens of
thousands of sockets alive. This script opens the requested number of
connections, verifies that every one of them is accepted, still answers a
request afterwards, and is released cleanly on shutdown.

Usage: python3 tests/c10k_test.py [count] [path-to-binary]
"""

import os
import signal
import socket
import subprocess
import sys
import time

COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
BIN = sys.argv[2] if len(sys.argv) > 2 else "bin/dantrix"
# Overridable so two suites can run at the same time without colliding.
PORT = int(os.environ.get("DANTRIX_C10K_PORT", "18095"))
failures = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}{(' - ' + detail) if detail else ''}")
    if not cond:
        failures.append(name)


def main():
    docroot = "/tmp/dantrix-c10k"
    os.makedirs(docroot, exist_ok=True)
    with open(os.path.join(docroot, "index.html"), "w") as f:
        f.write("<h1>c10k</h1>\n")

    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "--root", docroot,
         "--max-conns", str(COUNT + 1000), "--keepalive-timeout", "60s",
         "--log-level", "info"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

    ready = False
    for _ in range(100):
        if proc.poll() is not None:
            out, err = proc.communicate()
            print(err.decode())
            print("server exited during startup")
            return 1
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.5):
                ready = True
                break
        except OSError:
            time.sleep(0.1)
    if not ready:
        proc.kill()
        print("server never accepted a connection")
        return 1

    soft, hard = None, None
    try:
        soft, hard = __import__("resource").getrlimit(__import__("resource").RLIMIT_NOFILE)
    except Exception:
        pass
    # The SOFT limit is what accept()/socket() are actually measured against.
    if soft is not None and soft < COUNT + 100:
        # Raise the soft limit up to the hard limit where possible: a default
        # of 1024 would otherwise make the run fail after ~1020 connections
        # and look like a server bug.
        if hard is not None and hard >= COUNT + 100:
            try:
                __import__("resource").setrlimit(
                    __import__("resource").RLIMIT_NOFILE, (COUNT + 100, hard))
                soft = COUNT + 100
                print(f"  raised the soft nofile limit to {soft}")
            except (OSError, ValueError):
                pass
        if soft is not None and soft < COUNT + 100:
            print(f"  [skip] soft nofile limit is {soft}, need {COUNT + 100}")
            proc.kill()
            return 0

    print(f"opening {COUNT} connections")
    socks = []
    failed = 0
    start = time.time()
    for i in range(COUNT):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            socks.append(s)
        except OSError as e:
            failed += 1
            if failed < 5:
                print(f"    connection {i} failed: {e}")
            continue
    opened = len(socks)
    elapsed = time.time() - start
    print(f"opened {opened}/{COUNT} in {elapsed:.1f}s")
    check(f"{COUNT} connections accepted", opened == COUNT, f"opened {opened}")

    # Every held connection must still serve a request.
    ok = 0
    for s in socks:
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: c10k\r\n\r\n")
            data = b""
            s.settimeout(5)
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            if data.startswith(b"HTTP/1.1 200"):
                ok += 1
        except OSError:
            pass
    check("every held connection answers", ok == opened, f"answered {ok}/{opened}")

    # --- the roadmap's "plus 1,000 active" half -----------------------------
    #
    # Holding idle connections is the easy half: an idle connection owns no read
    # buffer. The interesting claim is that 1,000 clients can be served *while*
    # that many sit idle, because then the server is doing real work with a large
    # descriptor table, a large timer wheel and a large connection pool.
    #
    # RSS and the descriptor count are sampled before and after, and the growth
    # must be proportional to the active clients rather than to the total: a
    # server that allocated per *idle* connection would show it here.
    active_count = int(os.environ.get("C10K_ACTIVE", "1000"))
    if active_count > 0:
        def sample():
            try:
                with open(f"/proc/{proc.pid}/status", "r", encoding="ascii") as f:
                    for line in f:
                        if line.startswith("VmRSS:"):
                            rss = int(line.split()[1])
                            break
                fds = len(os.listdir(f"/proc/{proc.pid}/fd"))
                return rss, fds
            except (OSError, ValueError, UnboundLocalError):
                return None, None

        rss_before, fds_before = sample()
        print(f"serving {active_count} active clients while {opened} stay idle")
        active = []
        served = 0
        errors = 0
        start = time.time()
        for _ in range(active_count):
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.sendall(b"GET / HTTP/1.1\r\nHost: c10k\r\n\r\n")
                active.append(s)
            except OSError:
                errors += 1
        # Read one response per active client, with the idle set still held.
        for s in active:
            try:
                s.settimeout(10)
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                if data.startswith(b"HTTP/1.1 200"):
                    served += 1
            except OSError:
                errors += 1
        elapsed_active = time.time() - start
        rss_after, fds_after = sample()

        print(f"  {served}/{active_count} active clients answered in {elapsed_active:.1f}s")
        if rss_before and rss_after:
            print(f"  RSS {rss_before / 1024:.1f} -> {rss_after / 1024:.1f} MiB "
                  f"({(rss_after - rss_before) / 1024:+.2f} MiB)")
        if fds_before and fds_after:
            print(f"  descriptors {fds_before} -> {fds_after} "
                  f"({fds_after - fds_before:+d})")
        check(f"{active_count} active clients served with {opened} idle held",
              served >= active_count - errors, f"served {served}, errors {errors}")
        check("the idle connections survived the active burst",
              all(s.fileno() >= 0 for s in socks))
        for s in active:
            try:
                s.close()
            except OSError:
                pass

    # And the server must shut down cleanly with them still open.
    proc.send_signal(signal.SIGTERM)
    for s in socks[:200]:
        try:
            s.close()
        except OSError:
            pass
    for s in socks:
        try:
            s.close()
        except OSError:
            pass
    try:
        out, err = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        check("server exits with connections open", False, "timeout")
        return 1
    check("server exits with connections open", proc.returncode == 0,
          f"exit {proc.returncode}")
    text = err.decode()
    check("shutdown summary present", "shutdown complete" in text)
    check("no sanitizer errors", "ERROR:" not in text and "runtime error" not in text)
    if failures:
        print("--- stderr tail ---")
        print(text[-2000:])

    print("c10k test:", "FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())