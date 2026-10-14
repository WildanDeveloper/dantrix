#!/ usr / bin / env python3
"""Soak test: hold mixed load for a long time and fail on a LEAK, not on a spike.

The roadmap claims "a stable 1-hour stress test (RSS and fd count flat)". Until
now nothing verified it: `make stress` only printed instructions for a human to
run and watch, so the claim could not fail.

This test makes it measurable. It runs a mixed workload -- idle connections that
stay open plus a stream of real requests -- and samples RSS and the descriptor
count on a fixed interval. The verdict is based on the TREND of those samples,
not on the difference between the first and the last:

  * a slow leak (a few KiB per thousand requests) is invisible in a
    start-versus-end comparison, because normal allocator churn is larger than
    the leak over a short run, but it is unmistakable as a slope over many
    samples;
  * a one-off spike (the pool growing to its steady-state size) is a step, not a
    slope, and must not be reported as a leak.

The slope is computed by least squares over the samples, and the test fails when
the fitted growth over the run exceeds a threshold that scales with the run
length, so a short run is not held to the same absolute standard as a long one.

Usage:
    python3 tests/soak_test.py [--seconds N] [--clients N] [binary]

    --seconds   how long to run (default 3600, the roadmap's one hour)
    --clients   idle connections to hold (default 200)

Environment:
    DANTRIX_SOAK_PORT   port to use (default 18096)
    DANTRIX_SOAK_ROUNDS how many requests each active client sends per second
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

PORT = int(os.environ.get("DANTRIX_SOAK_PORT", "18096"))
SAMPLE_INTERVAL = 5.0  # seconds between samples

failures = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def read_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status", "r", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def read_fd_count(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return None

#A trend verdict needs enough samples AND enough absolute movement to be
#meaningful.A slope fitted over 60 seconds from a few MiB of allocator noise
#can read as hundreds of MiB / hour while the process is, in fact, flat : the
#server below sat at exactly 2176 kB for a full minute.Requiring both a
#minimum run length and a minimum absolute change keeps a short smoke run from
#producing a verdict that is really about its own noise.
MIN_SECONDS_FOR_TREND = 600.0     # 10 minutes
MIN_RSS_CHANGE_KB = 4096.0        # 4 MiB end to end
MIN_FD_CHANGE = 16.0              # 16 descriptors end to end


def slope_per_hour(samples):
    """Least-squares slope of (seconds, value) in units per hour.

    Using a fit rather than first-minus-last makes the verdict insensitive to a
    single noisy sample at either end, which is exactly the failure mode of a
    naive comparison.
    """
    n = len(samples)
    if n < 3:
        return 0.0
    mean_x = sum(s[0] for s in samples) / n
    mean_y = sum(s[1] for s in samples) / n
    num = sum((s[0] - mean_x) * (s[1] - mean_y) for s in samples)
    den = sum((s[0] - mean_x) ** 2 for s in samples)
    if den == 0.0:
        return 0.0
    return (num / den) * 3600.0


class Workload:
    """Idle connections held open, plus clients that keep sending requests."""

    def __init__(self, port, idle_count):
        self.port = port
        self.idle_count = idle_count
        self.idle = []
        self.requests = 0
        self.errors = 0
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.threads = []

    def open_idle(self):
        """Hold connections open with one request each, then leave them idle."""
        for _ in range(self.idle_count):
            if self.stop.is_set():
                break
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
                s.settimeout(5)
#Read the response so the connection is a clean idle keep - alive.
                buf = b""
                while b"\r\n\r\n" not in buf:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                self.idle.append(s)
            except OSError:
                with self.lock:
                    self.errors += 1
                break

    def active_client(self, seconds, per_second):
        """Send requests at a steady rate, reconnecting if the server closes."""
        while not self.stop.is_set():
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            except OSError:
                with self.lock:
                    self.errors += 1
                time.sleep(0.1)
                continue
            try:
                s.settimeout(5)
                for _ in range(per_second * int(seconds) + 10):
                    if self.stop.is_set():
                        return
                    s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\n\r\n")
                    buf = b""
                    while b"\r\n\r\n" not in buf:
                        chunk = s.recv(65536)
                        if not chunk:
                            raise OSError("closed")
                        buf += chunk
                    head, _, rest = buf.partition(b"\r\n\r\n")
                    length = 0
                    for line in head.split(b"\r\n"):
                        if line.lower().startswith(b"content-length:"):
                            length = int(line.split(b":", 1)[1])
                    while len(rest) < length:
                        chunk = s.recv(65536)
                        if not chunk:
                            raise OSError("closed")
                        rest += chunk
                    with self.lock:
                        self.requests += 1
                    time.sleep(1.0 / max(1, per_second))
            except OSError:
                with self.lock:
                    self.errors += 1
            finally:
                try:
                    s.close()
                except OSError:
                    pass

    def start(self, seconds, active_clients, per_second):
        self.open_idle()
        for _ in range(active_clients):
            t = threading.Thread(target=self.active_client, args=(seconds, per_second), daemon=True)
            t.start()
            self.threads.append(t)

    def finish(self):
        self.stop.set()
        for t in self.threads:
            t.join(timeout=5)
        for s in self.idle:
            try:
                s.close()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=3600, help="run length (default 3600)")
    ap.add_argument("--clients", type=int, default=200, help="idle connections (default 200)")
    ap.add_argument("--active", type=int, default=4, help="active clients (default 4)")
    ap.add_argument("--rate", type=int, default=50, help="requests/sec per active client")
    ap.add_argument("binary", nargs="?", default="bin/dantrix")
    args = ap.parse_args()

    root = tempfile.mkdtemp(prefix="dantrix-soak-")
    with open(os.path.join(root, "f"), "w", encoding="ascii") as f:
        f.write("x" * 1024)
    log = os.path.join(root, "err.log")

    proc = subprocess.Popen(
        [args.binary, "-b", "127.0.0.1", "-p", str(PORT), "-r", root, "-w", "2",
         "--access-log", "off", "--error-log", log, "--status-path", "/status",
         "--max-conns", "4096",
#Hold idle connections for the whole run : with the default 30s the
#server closes them mid - soak, which looks like the descriptor count
#dropping and has nothing to do with a leak.
         "-o", f"keepalive_timeout={args.seconds + 60}s"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_ready():
        for _ in range(100):
            if proc.poll() is not None:
                return False
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.1)
        return False

    if not wait_ready():
        print("soak: the server did not start")
        proc.kill()
        return 1

    print(f"soak: {args.seconds}s, {args.clients} idle connections, "
          f"{args.active} active clients at ~{args.rate} req/s each")

    wl = Workload(PORT, args.clients)
    t0 = time.time()
    wl.start(args.seconds, args.active, args.rate)

    rss_samples = []
    fd_samples = []
    believed_samples = []

    try:
        while time.time() - t0 < args.seconds:
            elapsed = time.time() - t0
            rss = read_rss_kb(proc.pid)
            fds = read_fd_count(proc.pid)
            if rss is not None:
                rss_samples.append((elapsed, float(rss)))
            if fds is not None:
                fd_samples.append((elapsed, float(fds)))
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/status", timeout=5) as r:
                    des = json.load(r)["descriptors"]
                believed_samples.append((elapsed, float(des["fds_believed"]),
                                         float(des["fds_actual"])))
            except (OSError, KeyError, ValueError):
                pass
            time.sleep(SAMPLE_INTERVAL)
    except KeyboardInterrupt:
        print("soak: interrupted")
    finally:
        elapsed = time.time() - t0
        wl.finish()

    with wl.lock:
        requests = wl.requests
        errors = wl.errors

#Liveness must be checked while the server is still running : asking it to
#stop first and then connecting would always fail.
    alive_ok = False
    alive_detail = ""
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        s.settimeout(3)
        data = s.recv(200)
        s.close()
        alive_ok = data.startswith(b"HTTP/1.1 200")
        alive_detail = data[:40].decode("latin1", "replace")
    except OSError as exc:
        alive_detail = str(exc)

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    print()
    rate = requests / elapsed if elapsed > 0 else 0.0
    target = args.active * args.rate
    print(f"soak: ran {elapsed:.0f}s, {requests} requests ({rate:.0f}/s, target "
          f"{target}/s), {errors} client errors, {len(rss_samples)} samples")

    if len(rss_samples) < 3:
        check("soak: enough samples to judge a trend", False,
              f"only {len(rss_samples)} samples")
        return 1

    # --- RSS trend ---------------------------------------------------------
    rss_first = rss_samples[0][1]
    rss_last = rss_samples[-1][1]
    rss_slope = slope_per_hour(rss_samples)
    # The budget separates "start-up noise" from "a leak". Start-up noise is a
    # one-off of a few MiB that a short run cannot average away, so the floor is
    # 4 MiB of *fitted* growth per hour. A real per-request leak is proportional
    # to the run: at the default rate this workload does ~10^5 requests an hour,
    # so even 64 bytes leaked per request would be ~6 MiB/hour on a long run.
    rss_budget_kb = max(4096.0, 4096.0 * (elapsed / 3600.0))
    rss_change = rss_last - rss_first
    print(f"soak: RSS {rss_first / 1024:.1f} -> {rss_last / 1024:.1f} MiB "
          f"(change {rss_change / 1024:+.2f} MiB), fitted slope "
          f"{rss_slope / 1024:+.2f} MiB/hour (budget {rss_budget_kb / 1024:.2f} MiB)")
    if elapsed < MIN_SECONDS_FOR_TREND:
        print(f"soak: note - run shorter than {MIN_SECONDS_FOR_TREND / 60:.0f} minutes, "
              f"so a slope verdict would be dominated by its own noise; judging on "
              f"absolute change instead (a leak of {MIN_RSS_CHANGE_KB / 1024:.0f} MiB "
              f"in {elapsed:.0f}s would still fail)")
        check("soak: RSS does not grow over the run",
              rss_change <= MIN_RSS_CHANGE_KB,
              f"grew {rss_change / 1024:+.2f} MiB in {elapsed:.0f}s")
    else:
        check("soak: RSS does not grow over the run", rss_slope <= rss_budget_kb,
              f"fitted {rss_slope / 1024:+.2f} MiB/hour exceeds "
              f"{rss_budget_kb / 1024:.2f} MiB")

    # --- descriptor trend --------------------------------------------------
    fd_first = fd_samples[0][1]
    fd_last = fd_samples[-1][1]
    fd_slope = slope_per_hour(fd_samples)
    fd_change = fd_last - fd_first
    print(f"soak: fds {fd_first:.0f} -> {fd_last:.0f} (change {fd_change:+.0f}), "
          f"fitted slope {fd_slope:+.1f}/hour")
    if elapsed < MIN_SECONDS_FOR_TREND:
        check("soak: the descriptor count does not grow", fd_change <= MIN_FD_CHANGE,
              f"grew {fd_change:+.0f} descriptors in {elapsed:.0f}s")
    else:
        check("soak: the descriptor count does not grow", fd_slope <= 10.0,
              f"fitted {fd_slope:+.1f} fds/hour")

    # --- the accounting invariant must hold --------------------------------
    if believed_samples:
        worst = max(abs(b - a) for _, b, a in believed_samples)
        gap_first = believed_samples[0][2] - believed_samples[0][1]
        gap_last = believed_samples[-1][2] - believed_samples[-1][1]
        print(f"soak: fds_actual - fds_believed {gap_first:.0f} -> {gap_last:.0f} "
              f"(constant process-wide descriptors)")
        check("soak: the believed/actual descriptor gap stays constant",
              abs(gap_last - gap_first) <= 2.0,
              f"gap moved from {gap_first:.0f} to {gap_last:.0f}")
        check("soak: no descriptor went unaccounted", worst < 64.0,
              f"worst gap {worst:.0f}")

    # --- the server must still work ----------------------------------------
    check("soak: the server still serves after the run", alive_ok, alive_detail)

    check("soak: no client errors beyond connection churn", errors < requests // 100,
          f"{errors} errors for {requests} requests")

    text = open(log, encoding="utf-8", errors="replace").read()
    check("soak: no sanitizer error in the log",
          "AddressSanitizer" not in text and "runtime error:" not in text)

    print()
    if failures:
        print(f"soak test: FAILED ({len(failures)} checks)")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("soak test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
