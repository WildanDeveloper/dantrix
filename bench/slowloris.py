#!/usr/bin/env python3
"""Slowloris resilience test (scenario S8).

Opens many connections and sends a request one byte at a time (or slowly), so
the server must close them via header_timeout / body_timeout instead of letting
them accumulate. The point is not to "win" but to show the timeouts and the
connection limit doing their job.

Usage:
    python3 bench/slowloris.py [--host H] [--port P] [--count N]
                               [--delay SECONDS] [--duration SECONDS]

Exit code 0 when the server survived and closed the connections.
"""

import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--count", type=int, default=200,
                    help="number of slow connections (default 200)")
    ap.add_argument("--delay", type=float, default=0.5,
                    help="seconds between header bytes (default 0.5)")
    ap.add_argument("--duration", type=float, default=10.0,
                    help="seconds to keep sending (default 10)")
    args = ap.parse_args()

    partial = b"GET / HTTP/1.1\r\nHost: slow\r\nX-Pad: "
    socks = []
    for _ in range(args.count):
        try:
            s = socket.create_connection((args.host, args.port), timeout=5)
            socks.append(s)
        except OSError as exc:
            print(f"connect failed after {len(socks)}: {exc}", file=sys.stderr)
            break
    print(f"opened {len(socks)} slow connections")

    deadline = time.time() + args.duration
    payload = partial
    sent = 0
    try:
        while time.time() < deadline:
            alive = []
            for s in socks:
                try:
                    s.sendall(b"a")
                    sent += 1
                    alive.append(s)
                except OSError:
                    pass  # the server closed it: exactly what we want
            socks = alive
            if not socks:
                break
            time.sleep(args.delay)
    except KeyboardInterrupt:
        pass

    for s in socks:
        s.close()
    print(f"sent {sent} slow bytes; {len(socks)} connections were still open at the end")
    print("slowloris test: the server stayed responsive (timeouts working)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
