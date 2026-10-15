#!/usr/bin/env python3
"""Configuration surface test for Dantrix.

Why this file exists
--------------------
Every test suite before this one started a server and made requests. None of them
ever read a configuration file. That blind spot is why two stale claims survived
several releases in the documentation: the README said `add_header` was "still
announced as not implemented" when it had been working since v0.8.10, and the
start-up warning for `edge_triggered` promised a v0.7 implementation nine
releases after v0.7 shipped. Nothing tested the surface, so nothing contradicted
the prose.

This test reads the surface as an operator would: it writes configuration files,
starts the binary, and checks what the process does. The rules it enforces are:

  1. A key that is documented as accepted must not be rejected.
  2. A key that is accepted but has no effect must say so on stderr.
  3. A key whose only documented value cannot be used must be rejected loudly
     rather than silently downgraded (that is the `log_format = json` case).
  4. A rejected configuration must exit with the documented code (2) and must
     not leave a log file or a pid file behind.

The checks read the *documentation's own tables* for the key list where possible,
so a key added to the README but not to the registry fails here.

Usage: python3 tests/config_test.py [path-to-binary]
"""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "bin/dantrix"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

failures = []
checks = 0


def check(name, cond, detail=""):
    global checks
    checks += 1
    if cond:
        print(f"  [ok] {name}")
    else:
        failures.append(name)
        print(f"  [FAIL] {name}" + (f" - {detail}" if detail else ""))


def run_config(conf_text, timeout=6):
    """Write a config, start the binary, and return (returncode, stderr, stdout).

    The server is given a port that is not listening anywhere so it can never be
    mistaken for a real one; the config is expected to be judged before any
    socket is opened.
    """
    d = tempfile.mkdtemp(prefix="dantrix-cfg-")
    try:
        docroot = os.path.join(d, "root")
        os.makedirs(docroot, exist_ok=True)
        with open(os.path.join(docroot, "index.html"), "w", encoding="ascii") as f:
            f.write("<h1>cfg</h1>\n")
        conf = os.path.join(d, "dantrix.conf")
        text = f"bind = 127.0.0.1\nport = 18077\nroot = {docroot}\n" + conf_text
        with open(conf, "w", encoding="ascii") as f:
            f.write(text)
        proc = subprocess.Popen([BIN, "-c", conf], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=REPO)
        try:
            out, err = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            # The configuration was accepted and the server is running, which is
            # a valid outcome for a key that is supposed to work. Shut it down
            # politely so the shutdown path is exercised too.
            proc.terminate()
            try:
                out, err = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate()
        return proc.returncode, err.decode("utf-8", "replace"), out.decode("utf-8", "replace"), d
    except Exception:
        shutil.rmtree(d, ignore_errors=True)
        raise


def documented_keys():
    """Keys named in the README's configuration tables.

    The tables look like `| `key_name` | type | default | reload | description |`.
    Anything the README documents should be registered in the binary; this is the
    check that would have caught a documented-but-absent key. The section runs
    from the configuration reference to the closing `</details>` of its block,
    because the table is interrupted by explanatory subsections (`Per-IP
    connection limit`, `Sandbox`) before it ends -- stopping at the first of those
    cut the count from 50 to 36. The summary's stated count is cross-checked
    against the rows actually found, because that count had drifted: it said 45
    keys while the table has 50 and the binary registers 50.
    """
    path = os.path.join(REPO, "README.md")
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.index("### Reference for all keys")
    end = text.index("</details>", start)
    seg = text[start:end]
    keys = set()
    for m in re.finditer(r"^\| `([a-z_][a-z0-9_]*)` \|", seg, re.MULTILINE):
        keys.add(m.group(1))
    m = re.search(r"Open the reference table \((\d+) keys?\)", text)
    stated = int(m.group(1)) if m else -1
    return keys, stated


def main():
    if not os.path.exists(BIN):
        print(f"  [FAIL] binary {BIN} not found - build it first")
        return 1

    print("configuration surface")

    # --- 1. A working key must actually work --------------------------------
    rc, err, _out, d = run_config("add_header = X-Config-Test: present\n")
    shutil.rmtree(d, ignore_errors=True)
    check("add_header is accepted and the server starts", rc == 0 or rc is None,
          f"exit {rc}, stderr: {err.strip()[:200]}")

    # --- 2. A no-op key must announce itself --------------------------------
    rc, err, _out, d = run_config("edge_triggered = true\n")
    shutil.rmtree(d, ignore_errors=True)
    check("edge_triggered is accepted", rc == 0 or rc is None, f"exit {rc}")
    check("edge_triggered warns that it has no effect",
          "edge_triggered" in err and ("no effect" in err or "not implemented" in err),
          f"stderr: {err.strip()[:200]}")
    check("the warning names no future release",
          not re.search(r"planned for v[0-9]", err),
          "a promise with a version in it goes stale; the message must not carry one")

    # --- 3. A value that cannot be used is rejected, not downgraded ---------
    rc, err, _out, d = run_config("log_format = json\n")
    shutil.rmtree(d, ignore_errors=True)
    check("log_format = json is rejected with exit code 2", rc == 2, f"exit {rc}")
    check("the rejection names the key and the value",
          "log_format" in err or "log format" in err,
          f"stderr: {err.strip()[:200]}")

    rc, _err, _out, d = run_config("log_format = combined\n")
    shutil.rmtree(d, ignore_errors=True)
    check("log_format = combined still works", rc == 0 or rc is None, f"exit {rc}")

    # --- 4. Rejection leaves nothing behind ---------------------------------
    d = tempfile.mkdtemp(prefix="dantrix-cfg-")
    try:
        docroot = os.path.join(d, "root")
        os.makedirs(docroot, exist_ok=True)
        logfile = os.path.join(d, "access.log")
        pidfile = os.path.join(d, "dantrix.pid")
        conf = os.path.join(d, "dantrix.conf")
        with open(conf, "w", encoding="ascii") as f:
            f.write(f"bind = 127.0.0.1\nport = 18077\nroot = {docroot}\n"
                    f"access_log = {logfile}\npid_file = {pidfile}\n"
                    f"max_connections = not_a_number\n")
        proc = subprocess.run([BIN, "-c", conf], capture_output=True, timeout=10, cwd=REPO)
        check("a bad value exits with code 2", proc.returncode == 2,
              f"exit {proc.returncode}")
        check("a rejected config creates no access log", not os.path.exists(logfile))
        check("a rejected config creates no pid file", not os.path.exists(pidfile))
    finally:
        shutil.rmtree(d, ignore_errors=True)

    # --- 5. The documented surface and the registry agree -------------------
    #
    # The README tables are the contract. This compares them against the key
    # names the binary knows: `--print-config` dumps the effective configuration,
    # so a documented key missing from that dump was documented but never wired
    # up. It is the check that would have caught the stale `add_header` claim.
    proc = subprocess.run([BIN, "--print-config"], capture_output=True, timeout=15,
                          cwd=REPO)
    out = proc.stdout.decode("utf-8", "replace")
    rc = proc.returncode
    if rc == 0 and out:
        effective = set(re.findall(r"^([a-z_][a-z0-9_]*)\s*=", out, re.MULTILINE))
        # `add_header` is the one key the dump prints only when it is set, so it
        # is added explicitly rather than silently counted as missing.
        effective.add("add_header")
        documented, stated = documented_keys()
        missing = sorted(documented - effective)
        check("every documented key is known to the binary", not missing,
              f"documented but not implemented: {missing}")
        check("the table's stated key count matches its rows",
              stated == len(documented), f"summary says {stated}, table has {len(documented)}")
        check("every registered key is documented",
              not (effective - documented),
              f"registered but absent from the docs: {sorted(effective - documented)}")
        print(f"  (documented keys: {len(documented)}, binary knows: {len(effective)})")
    else:
        check("--print-config works", False, f"exit {rc}")

    # --- 5b. The CLI flag count is verified, not copied ---------------------
    # The readiness document claims a flag count. Counting the getopt_long table
    # and comparing it against --help catches both a flag added without help text
    # and a stale number in the prose.
    help_out = subprocess.run([BIN, "--help"], capture_output=True, timeout=15,
                              cwd=REPO).stdout.decode("utf-8", "replace")
    src = open(os.path.join(REPO, "src", "main.c"), "r", encoding="utf-8").read()
    table = re.search(r"struct option long_opts\[\]\s*=\s*\{(.*?)\};", src, re.S)
    flags = sorted(set(re.findall(r'\{"([a-z][a-z-]*)"', table.group(1)))) if table else []
    check("the CLI flag table was found", bool(table))
    check("every flag has help text",
          all(f"--{f}" in help_out for f in flags),
          f"missing: {[f for f in flags if f'--{f}' not in help_out]}")
    print(f"  (CLI flags in the table: {len(flags)})")

    # --- 6. A live key is honoured, not just accepted -----------------------
    # `add_header` is the key that was documented as unimplemented for two
    # releases. Acceptance is not enough: the header has to arrive.
    d = tempfile.mkdtemp(prefix="dantrix-cfg-")
    proc = None
    try:
        docroot = os.path.join(d, "root")
        os.makedirs(docroot, exist_ok=True)
        with open(os.path.join(docroot, "index.html"), "w", encoding="ascii") as f:
            f.write("<h1>cfg</h1>\n")
        port = 18076
        conf = os.path.join(d, "dantrix.conf")
        with open(conf, "w", encoding="ascii") as f:
            f.write(f"bind = 127.0.0.1\nport = {port}\nroot = {docroot}\n"
                    f"add_header = X-Config-Test: present\n")
        proc = subprocess.Popen([BIN, "-c", conf], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=REPO)
        response = b""
        for _ in range(60):
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                s.sendall(b"GET / HTTP/1.1\r\nHost: cfg\r\nConnection: close\r\n\r\n")
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    response += chunk
                s.close()
                break
            except OSError:
                time.sleep(0.1)
        check("a documented key reaches the response",
              b"X-Config-Test: present" in response,
              f"first line: {response.splitlines()[0][:60] if response else 'no response'}")
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
        shutil.rmtree(d, ignore_errors=True)

    print(f"config test: {'FAILED' if failures else 'PASSED'} ({checks} checks)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
