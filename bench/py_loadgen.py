#!/usr/bin/env python3
"""Minimal keep-alive load generator, used to fill the README benchmark tables
on machines without wrk/ab.

This is NOT a replacement for wrk: it is single-process Python, so it measures
a lower bound and its own overhead becomes the ceiling quickly. It is honest
about that: the reported number is "requests this client could push", not "the
server's maximum". Use wrk/wrk2 for the real tables.

Usage:
    python3 bench/py_loadgen.py --url http://127.0.0.1:8080/1k.bin \
        --conns 32 --duration 5
"""

import argparse
import socket
import threading
import time
from urllib.parse import urlparse

counts = []
lock = threading.Lock()
stop = threading.Event()


def worker(host, port, path, duration):
    sent = 0
    try:
        while not stop.is_set():
            s = socket.create_connection((host, port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            req = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\n"
                   "Connection: keep-alive\r\n\r\n").encode()
            s.sendall(req)
            deadline = time.time() + duration
            while not stop.is_set() and time.time() < deadline:
                s.sendall(req)
                sent += 1
                # Read one response: header + body, to keep the socket in sync.
                buf = b""
                need = None
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        raise OSError("closed")
                    buf += chunk
                    if need is None and b"\r\n\r\n" in buf:
                        head, _, rest = buf.partition(b"\r\n\r\n")
                        length = 0
                        for line in head.split(b"\r\n"):
                            if line.lower().startswith(b"content-length:"):
                                length = int(line.split(b":", 1)[1])
                        need = length - len(rest)
                    elif need is not None:
                        need -= len(chunk)
                    if need is not None and need <= 0:
                        break
            s.close()
    except OSError:
        pass
    with lock:
        counts.append(sent)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://127.0.0.1:8080/1k.bin")
    ap.add_argument("--conns", type=int, default=32)
    ap.add_argument("--duration", type=float, default=5.0)
    args = ap.parse_args()

    u = urlparse(args.url)
    host = u.hostname or "127.0.0.1"
    port = u.port or 80
    path = u.path or "/"

    threads = [threading.Thread(target=worker, args=(host, port, path, args.duration))
               for _ in range(args.conns)]
    start = time.time()
    for t in threads:
        t.start()
    time.sleep(args.duration)
    stop.set()
    for t in threads:
        t.join(timeout=10)
    elapsed = time.time() - start

    total = sum(counts)
    print(f"url={args.url} conns={args.conns} duration={elapsed:.1f}s")
    print(f"requests={total}  requests/sec={total / elapsed:.0f}")
    print("note: single-process Python client; the real ceiling needs wrk/wrk2")


if __name__ == "__main__":
    main()
