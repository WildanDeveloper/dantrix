#!/usr/bin/env python3
"""Try to FALSIFY the TOCTOU claim by racing the filesystem against the server.

SECURITY.md says the document root is enforced atomically because there is no
check-then-open: `openat2(RESOLVE_BENEATH)` resolves and opens in one kernel
call, so there is no window between "is this inside the root" and "open it".
That claim was previously marked "verified by construction, not falsified",
which is honest but weak. This test attacks it.

The attack: a writer thread swaps the target between
  - a legitimate file inside the root, and
  - a symlink pointing at a secret OUTSIDE the root,
as fast as it can, while a reader thread requests the path in a loop. If the
server ever has a check-then-open window, some request will land in it and the
secret will appear in a response.

Run: python3 tests/toctou_test.py [binary]
Exit code 0 when no request ever leaked, non-zero when one did.
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "bin/dantrix"
PORT = int(os.environ.get("DANTRIX_TEST_PORT", "18101")) + 110
SECRET = b"TOCTOU-SECRET-MUST-NEVER-ESCAPE"
LEGIT = b"legitimate-content"
SWAP_SECONDS = float(os.environ.get("TOCTOU_SECONDS", "10"))


def main():
    work = tempfile.mkdtemp(prefix="dantrix-toctou-")
    root = os.path.join(work, "root")
    os.makedirs(root, exist_ok=True)

    secret_path = os.path.join(work, "secret.txt")
    with open(secret_path, "wb") as f:
        f.write(SECRET)

    target = os.path.join(root, "target")
    with open(target, "wb") as f:
        f.write(LEGIT)

    # A second shape: a directory entry swapped between a real directory and a
    # symlink to the outside, to exercise the index-file resolution path too.
    index_target = os.path.join(root, "idx")
    os.makedirs(index_target, exist_ok=True)
    with open(os.path.join(index_target, "index.html"), "wb") as f:
        f.write(LEGIT)

    proc = subprocess.Popen(
        [BIN, "-b", "127.0.0.1", "-p", str(PORT), "-r", root, "-w", "2",
         "--access-log", "off", "--error-log", "/dev/null"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def ready():
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.1)
        return False

    if not ready():
        print("toctou: server did not start")
        proc.kill()
        shutil.rmtree(work, ignore_errors=True)
        return 1

    stop = threading.Event()
    leaks = []
    requests = [0]
    swaps = [0]

    def swapper():
        """Swap the target between a real file and an escaping symlink."""
        while not stop.is_set():
            try:
                # Point at the outside secret.
                os.unlink(target)
                os.symlink(secret_path, target)
                swaps[0] += 1
                # Back to a legitimate file.
                os.unlink(target)
                with open(target, "wb") as f:
                    f.write(LEGIT)
                swaps[0] += 1
            except OSError:
                # The server may be holding the name; retry on the next turn.
                continue
            # Also swap the directory entry.
            try:
                os.unlink(os.path.join(root, "idx_link"))
            except OSError:
                pass
            try:
                os.symlink(secret_path, os.path.join(root, "idx_link"))
            except OSError:
                pass

    def requester(path):
        while not stop.is_set():
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
            except OSError:
                continue
            try:
                s.sendall(b"GET " + path + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                s.settimeout(2)
                data = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                    if SECRET in data:
                        leaks.append((path.decode(), "during read"))
                        return
                requests[0] += 1
                if SECRET in data:
                    leaks.append((path.decode(), "in body"))
                    return
            except OSError:
                pass
            finally:
                s.close()

    threads = [threading.Thread(target=swapper, daemon=True)]
    for path in (b"/target", b"/target", b"/target", b"/idx_link"):
        threads.append(threading.Thread(target=requester, args=(path,), daemon=True))
    for t in threads:
        t.start()

    time.sleep(SWAP_SECONDS)
    stop.set()
    for t in threads:
        t.join(timeout=3)

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    shutil.rmtree(work, ignore_errors=True)

    print(f"toctou: {requests[0]} requests against {swaps[0]} filesystem swaps "
          f"over {SWAP_SECONDS:.0f}s")
    if leaks:
        print(f"toctou: FAILED - the secret escaped via {leaks[0][0]} ({leaks[0][1]})")
        return 1
    print("toctou: PASSED - no request ever observed the outside file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
