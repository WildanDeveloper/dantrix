#!/usr/bin/env python3
"""Run the README benchmark scenarios and print a table ready to paste.

This is the harness the README's results section refers to. It measures with
`wrk` when it is available (the real tool) and falls back to the Python client
with an explicit warning, because the Python client is its own bottleneck and
must never be reported as the server's ceiling.

Every number is printed with the environment it was measured in, so a result
cannot be quoted without its context.

Usage:
    python3 bench/measure.py [--duration 10s] [--conns 256] [--port 8080]
                             [--binary bin/dantrix] [--out FILE] [--save]

`--save` writes the README's reproducible record: a directory
`bench/results/<date>-<commit>/` holding `results.md` (the table printed here),
`environment.txt` (hardware, kernel, compiler, binary, commit) and
`command.txt` (the exact command line, so the run can be repeated). The README
promises that a result is never quoted without its context; this is the code
that keeps the promise.

The scenarios match the README: S1 (small file, keep-alive), S3 (large file),
S4 (high concurrency), plus an A/B of open_file_cache, which is the change that
removed the open and stat from every request.
"""

import argparse
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def have(tool):
    return shutil.which(tool) is not None


def wrk_requests_per_sec(url, threads, conns, duration, extra=None):
    cmd = ["wrk", f"-t{threads}", f"-c{conns}", f"-d{duration}", "--latency"]
    if extra:
        cmd += extra
    cmd.append(url)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=300).stdout
    m = re.search(r"Requests/sec:\s+([\d.]+)", out)
    lat = re.search(r"50%\s+([\d.]+)(\w+)", out)
    p99 = re.search(r"99%\s+([\d.]+)(\w+)", out)
    return {
        "rps": float(m.group(1)) if m else None,
        "p50": f"{lat.group(1)}{lat.group(2)}" if lat else "n/a",
        "p99": f"{p99.group(1)}{p99.group(2)}" if p99 else "n/a",
        "raw": out,
    }


def start_server(binary, port, root, extra=None):
    cmd = [binary, "-b", "127.0.0.1", "-p", str(port), "-r", root, "-w", "2",
           "--access-log", "off", "--error-log", "/dev/null"]
    if extra:
        cmd += extra
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if proc.poll() is not None:
            return None
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.1)
    proc.kill()
    return None


def stop_server(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def rss_of(pid):
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--duration", default="10s")
    ap.add_argument("--conns", type=int, default=256)
    ap.add_argument("--port", type=int, default=20800)
    ap.add_argument("--binary", default=os.path.join(ROOT, "bin", "dantrix"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--save", action="store_true",
                    help="write bench/results/<date>-<commit>/ (results, environment, command)")
    args = ap.parse_args()

    if not have("wrk"):
        print("measure: wrk is NOT installed. The Python client is its own")
        print("         bottleneck, so any number from it is a lower bound and")
        print("         must not be published as the server's ceiling.")
        print("         Install wrk (apt install wrk) and re-run for real numbers.")
        return 1

    root = os.path.join(HERE, "www")
    if not os.path.isdir(root) or not os.path.exists(os.path.join(root, "1k.bin")):
        print("measure: bench payloads missing; run bench/make_files.sh first")
        return 1

    uname = platform.uname()
    gcc = subprocess.run(["gcc", "--version"], capture_output=True, text=True).stdout
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    version = subprocess.run([args.binary, "--version"], capture_output=True,
                             text=True).stdout.strip()

    lines = []
    def emit(s=""):
        print(s)
        lines.append(s)

    emit("**Test environment (measured)**")
    emit()
    emit("| Item | Value |")
    emit("|---|---|")
    emit(f"| CPU | {platform.processor() or 'unknown'}, {os.cpu_count()} vCPU |")
    try:
        with open("/proc/cpuinfo", encoding="ascii") as f:
            for line in f:
                if line.startswith("model name"):
                    emit(f"| CPU model | {line.split(':', 1)[1].strip()} |")
                    break
    except OSError:
        pass
    try:
        mem = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1024 ** 3)
        emit(f"| RAM | {mem:.1f} GiB |")
    except (ValueError, OSError):
        pass
    emit(f"| Kernel | {uname.system} {uname.release} |")
    emit(f"| Compiler | {gcc.splitlines()[0] if gcc else 'unknown'} |")
    emit(f"| Binary | {version} (commit {commit}) |")
    emit(f"| Load generator | `wrk` ({'present' if have('wrk') else 'MISSING'}) |")
    emit(f"| Date | {time.strftime('%Y-%m-%d')} |")
    emit()

    # --- S1: small file, keep-alive, cache off then on ---------------------
    emit(f"**S1: 1 KiB file, keep-alive, {args.conns} connections ({args.duration})**")
    emit()
    emit("| Configuration | Req/s | p50 | p99 | RSS |")
    emit("|---|---|---|---|---|")

    for label, extra in (("cache off", None),
                         ("cache on (128 slots)", ["-o", "open_file_cache=128",
                                                   "-o", "open_file_cache_valid=60s"])):
        proc = start_server(args.binary, args.port, root, extra)
        if proc is None:
            emit(f"| {label} | server failed to start | | | |")
            continue
        try:
            # Warm up, then measure: the first run pays for cold page cache.
            subprocess.run(["wrk", "-t2", "-c64", "-d3s",
                            f"http://127.0.0.1:{args.port}/1k.bin"],
                           capture_output=True, timeout=120)
            r = wrk_requests_per_sec(f"http://127.0.0.1:{args.port}/1k.bin",
                                     4, args.conns, args.duration)
            rss = rss_of(proc.pid)
            emit(f"| {label} | {r['rps']:.0f} | {r['p50']} | {r['p99']} | "
                 f"{rss / 1024:.1f} MiB |" if r["rps"] else f"| {label} | n/a | | | |")
        finally:
            stop_server(proc)
    emit()

    # --- S3: large file ----------------------------------------------------
    emit(f"**S3: 1 MiB file, {args.duration}**")
    emit()
    emit("| Configuration | Req/s | Transfer/s | p99 |")
    emit("|---|---|---|---|")
    proc = start_server(args.binary, args.port, root,
                        ["-o", "open_file_cache=128", "-o", "open_file_cache_valid=60s"])
    if proc is not None:
        try:
            out = subprocess.run(["wrk", "-t4", "-c64", f"-d{args.duration}", "--latency",
                                  f"http://127.0.0.1:{args.port}/1m.bin"],
                                 capture_output=True, text=True, timeout=300).stdout
            rps = re.search(r"Requests/sec:\s+([\d.]+)", out)
            tr = re.search(r"Transfer/sec:\s+([\d.]+\w+)", out)
            p99 = re.search(r"99%\s+([\d.]+\w+)", out)
            emit(f"| cache on | {rps.group(1) if rps else 'n/a'} | "
                 f"{tr.group(1) if tr else 'n/a'} | {p99.group(1) if p99 else 'n/a'} |")
        finally:
            stop_server(proc)
    emit()

    # --- S4: high concurrency ---------------------------------------------
    emit(f"**S4: high concurrency, {args.duration}**")
    emit()
    emit("| Connections | Req/s | p50 | p99 |")
    emit("|---|---|---|---|")
    for conns in (1024, 4096):
        proc = start_server(args.binary, args.port, root,
                            ["-o", "open_file_cache=128", "-o", "open_file_cache_valid=60s",
                             "--max-conns", "8192"])
        if proc is None:
            emit(f"| {conns} | server failed to start | | |")
            continue
        try:
            r = wrk_requests_per_sec(f"http://127.0.0.1:{args.port}/1k.bin",
                                     4, conns, args.duration)
            emit(f"| {conns} | {r['rps']:.0f} | {r['p50']} | {r['p99']} |"
                 if r["rps"] else f"| {conns} | n/a | | |")
        finally:
            stop_server(proc)
    emit()
    emit("> Measured on loopback with the tool named above. Loopback has no NIC and")
    emit("> no packet loss, so these numbers compare code changes on one machine;")
    emit("> they do not predict production.")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print(f"\nmeasure: written to {args.out}")

    if args.save:
        # The README's reproducibility rule: results live in
        # bench/results/<date>-<commit>/ together with the hardware
        # specification and the configuration. Written last so a failed run
        # leaves no record that looks like a result.
        outdir = os.path.join(HERE, "results", f"{time.strftime('%Y-%m-%d')}-{commit or 'unknown'}")
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "results.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        with open(os.path.join(outdir, "environment.txt"), "w", encoding="utf-8") as f:
            f.write(f"date: {time.strftime('%Y-%m-%dT%H:%M:%S%z')}\n")
            f.write(f"commit: {commit}\n")
            f.write(f"binary: {version}\n")
            f.write(f"kernel: {uname.system} {uname.release}\n")
            f.write(f"machine: {uname.machine}\n")
            f.write(f"cpu_count: {os.cpu_count()}\n")
            f.write(f"compiler: {gcc.splitlines()[0] if gcc else 'unknown'}\n")
            f.write(f"load_generator: wrk ({'present' if have('wrk') else 'MISSING'})\n")
            f.write("server_flags: -b 127.0.0.1 -w 2 --access-log off --error-log /dev/null\n")
            f.write("server_overrides: open_file_cache=128 open_file_cache_valid=60s "
                    "(cache-on rows); none for cache-off rows\n")
        with open(os.path.join(outdir, "command.txt"), "w", encoding="utf-8") as f:
            f.write("python3 " + " ".join(sys.argv) + "\n")
        print(f"measure: saved to {os.path.relpath(outdir, ROOT)}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
