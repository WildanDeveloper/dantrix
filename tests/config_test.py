#!/usr/bin/env python3
"""Configuration surface test for Dantrix.

Why this file exists
--------------------
Every test suite before this one started a server and made requests. None of them
ever read a configuration file. That blind spot is why two stale claims survived
several releases in the documentation: the README said `add_header` was "still
announced as not implemented" when it had been working since v0.8.10, and the
start-up warning for `edge_triggered` promised an implementation in a release
that had already shipped. Nothing tested the surface, so nothing contradicted
the prose.

This test reads the surface as an operator would: it writes configuration files,
starts the binary, and checks what the process does. The rules it enforces are:

  1. A key that is documented as accepted must not be rejected.
  2. A key that was removed must be rejected loudly, not silently ignored
     (`edge_triggered`, deleted before the v1.0 freeze).
  3. A documented value must work (that is the `log_format = json` case, which
     used to be documented and rejected).
  4. A rejected configuration must exit with the documented code (2) and must
     not leave a log file or a pid file behind.
  5. A file the README embeds (the example configuration and every file in
     `deploy/`) must be the file on disk, byte for byte.
  6. A container example must mount the document root where the image serves it.
  7. The example configuration must validate and must differ from the compiled
     defaults exactly where its own header says it does.

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
# Same convention as the other standalone suites: the CI sets DANTRIX_TEST_PORT
# per job so parallel jobs cannot collide on a fixed port. The offsets keep the
# three ports this file uses apart from each other.
PORT_BASE = int(os.environ.get("DANTRIX_TEST_PORT", "18076"))

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
        text = f"bind = 127.0.0.1\nport = {PORT_BASE}\nroot = {docroot}\n" + conf_text
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


def fenced_blocks(text):
    """Every fenced code block in a Markdown document as (first line, body).

    Line numbers are 1-based and point at the first line of the body, so a
    failure can send the reader to the exact block.
    """
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].lstrip().startswith("```"):
                j += 1
            out.append((i + 1, "\n".join(lines[i + 1:j]).rstrip("\n")))
            i = j + 1
        else:
            i += 1
    return out


def config_values_match(conf_value, default_value):
    """Compare a value from a config file with the same key from --print-config.

    The two spellings differ by design: the dump prints durations in
    milliseconds (`10000ms`) where the file writes seconds (`10`), and sizes in
    bytes (`8192`) where the file may use suffixes (`8k`). Only equality is
    needed here, not canonicalisation.
    """
    if default_value.endswith("ms") and re.fullmatch(r"\d+", conf_value):
        return int(conf_value) * 1000 == int(default_value[:-2])
    m = re.fullmatch(r"(\d+)([kmg])", conf_value)
    if m and re.fullmatch(r"\d+", default_value):
        mult = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}[m.group(2)]
        return int(m.group(1)) * mult == int(default_value)
    return conf_value == default_value


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

    # --- 2. A key that was removed must be rejected, not ignored -----------
    # `edge_triggered` was accepted for nine releases, warned about, and never
    # implemented. Before the v1.0 freeze it was deleted: a switch that does
    # nothing is a trap. The removal is only real if setting it fails loudly.
    rc, err, _out, d = run_config("edge_triggered = true\n")
    shutil.rmtree(d, ignore_errors=True)
    check("the removed edge_triggered key is rejected", rc == 2, f"exit {rc}")
    check("the rejection names the key", "edge_triggered" in err,
          f"stderr: {err.strip()[:200]}")

    # --- 3. A documented value must work, not be rejected ------------------
    # `log_format = json` used to be documented and rejected with exit 2. It is
    # implemented now, so the test asserts both that it is accepted and that the
    # line it writes is valid JSON with the documented field names.
    rc, err, _out, d = run_config("log_format = json\n")
    shutil.rmtree(d, ignore_errors=True)
    check("log_format = json is accepted", rc == 0 or rc is None,
          f"exit {rc}, stderr: {err.strip()[:200]}")

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
            f.write(f"bind = 127.0.0.1\nport = {PORT_BASE}\nroot = {docroot}\n"
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

    # --- 5c. The directory tree in the README describes real files ----------
    # The tree claimed files that do not exist (CODE_OF_CONDUCT.md, .clang-tidy,
    # sanitizers.yml, four ADR files) for several releases, because it was written
    # as a target rather than a description and nothing checked it. This walks the
    # drawing and looks for every path.
    #
    # Three entries are git-ignored runtime directories (fuzz/corpus,
    # fuzz/artifacts, bench/results). They are marked in the tree as generated and
    # are exempt here: a fresh clone legitimately does not have them, and this
    # check runs on a fresh clone in CI. CI caught exactly that -- the check
    # failed there with ['artifacts', 'corpus', 'results'] while passing locally,
    # which is the useful failure: it proved the check reads the clone rather than
    # the author's working tree.
    readme = open(os.path.join(REPO, "README.md"), "r", encoding="utf-8").read()
    tree_start = readme.index("## Directory Structure")
    fence = readme.index("```text", tree_start)
    tree_end = readme.index("```", fence + 7)
    block = readme[fence:tree_end]
    generated = {"corpus", "artifacts", "results"}
    candidates = set()
    for m in re.finditer(r"([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)/?\s*(?:#.*)?$", block, re.M):
        name = m.group(1).strip()
        if name and name not in ("text", "dantrix") and name not in generated:
            candidates.add(name)
    dirs = [".", "src", "tests", "fuzz", "bench", "bench/tools", "conf", "deploy",
            "docs", "docs/adr", "examples", ".github/workflows", "tests/unit",
            ".github"]
    missing = []
    for name in sorted(candidates):
        if "/" in name:
            if not os.path.exists(os.path.join(REPO, name)):
                missing.append(name)
        elif not any(os.path.exists(os.path.join(REPO, d, name)) for d in dirs):
            missing.append(name)
    check("every path in the README's directory tree exists", not missing,
          f"claimed but absent: {missing}")
    check("the generated directories are marked as generated in the tree",
          all(f"{g}/" in block and "ignored" in block for g in generated))
    print(f"  (paths in the tree: {len(candidates)}, generated: {len(generated)})")

    # --- 5d. Every document that names a version names the current one ------
    # The status table said "Current release: v0.8.14" while the binary was at
    # 0.8.17, and the readiness document carried the same stale version. A
    # document that names the current release must name the one the binary
    # reports; historical mentions ("since v0.8.10", "before v0.8.3") are fine and
    # are not matched by this check, which only looks at the "current release"
    # pattern.
    version_out = subprocess.run([BIN, "--version"], capture_output=True, timeout=15,
                                 cwd=REPO).stdout.decode("utf-8", "replace")
    m = re.search(r"(\d+\.\d+\.\d+)", version_out)
    check("the binary reports a version", bool(m), version_out.strip()[:80])
    if m:
        current = m.group(1)
        for doc in ("README.md", "docs/v1.0-readiness.md"):
            text = open(os.path.join(REPO, doc), "r", encoding="utf-8").read()
            stale = re.findall(r"[Cc]urrent release[^\n|]*v(\d+\.\d+\.\d+)", text)
            stale += re.findall(r"[Cc]urrent release\s*\|\s*v(\d+\.\d+\.\d+)", text)
            check(f"{doc} names the current release as v{current}",
                  all(v == current for v in stale) if stale else True,
                  f"binary is {current}, document says {stale}")
            # The readiness document states which release it was reviewed at. It
            # is allowed to describe older releases as history (it does), so this
            # looks specifically for the review marker and requires it to name the
            # release the binary reports -- otherwise the document silently
            # describes a surface that has moved.
            marker = re.search(r"reviewed at v(\d+\.\d+\.\d+)", text)
            if marker:
                check(f"{doc}'s review marker names the current release",
                      marker.group(1) == current,
                      f"binary is {current}, marker says v{marker.group(1)}")

    # --- 5e. Every file the README embeds matches the file on disk ----------
    # The README embeds the example configuration and every file in deploy/ so
    # readers can see them without opening another tab. All six copies had
    # drifted: the embedded Dockerfile still said /var/www after the image moved
    # to /var/www/dantrix, the embedded logrotate file was missing the `su` and
    # `create` directives that make it work, and the embedded systemd unit was a
    # different design from the installed one. A copy that is not the file is
    # not documentation, it is a defect. The list of files is derived from the
    # deploy/ directory, so a new artifact must either be documented or fail
    # this check.
    blocks = fenced_blocks(readme)
    embedded_files = ["conf/dantrix.conf"]
    deploy_dir = os.path.join(REPO, "deploy")
    embedded_files += ["deploy/" + n for n in sorted(os.listdir(deploy_dir))
                       if os.path.isfile(os.path.join(deploy_dir, n))]
    for path in embedded_files:
        content = open(os.path.join(REPO, path), "r", encoding="utf-8").read().rstrip("\n")
        where = [lineno for lineno, body in blocks if body == content]
        first = content.split("\n", 1)[0]
        stale = [lineno for lineno, body in blocks
                 if body != content and body.split("\n", 1)[0] == first]
        check(f"the README embeds {path} byte-for-byte", bool(where),
              f"no fenced block equals the file; a stale copy starts near line(s) {stale}")

    # --- 5f. The container examples serve the image's document root ---------
    # Every `docker run` example and the compose file must mount the served
    # directory where the image looks for it. The quick start mounted
    # `$PWD/public` at /var/www while ENV DANTRIX_ROOT points at
    # /var/www/dantrix, so following it produced a server answering 404 for
    # everything. The root is read from the Dockerfile instead of repeated, so
    # the three files cannot disagree silently; mounting an ancestor of the root
    # (the old defect) leaves the root itself empty and is rejected too.
    dockerfile = open(os.path.join(REPO, "deploy", "Dockerfile"), "r", encoding="utf-8").read()
    m = re.search(r"^ENV DANTRIX_ROOT=(\S+)", dockerfile, re.MULTILINE)
    check("the image declares its document root", bool(m))
    if m:
        image_root = m.group(1).rstrip("\\")
        run_targets = []
        for lineno, body in blocks:
            if "docker run" not in body:
                continue
            for vm in re.finditer(r'-v\s+"([^"]+)"', body):
                parts = vm.group(1).split(":")
                if len(parts) >= 2:
                    run_targets.append((lineno, parts[1]))
        mounted = {t for _, t in run_targets}
        bad = sorted(t for t in mounted
                     if t != image_root and image_root.startswith(t.rstrip("/") + "/"))
        check("every docker run example mounts the document root into the image",
              bool(run_targets) and image_root in mounted and not bad,
              f"image serves {image_root}; mounts: {run_targets}")
        compose = open(os.path.join(REPO, "deploy", "docker-compose.yml"),
                       "r", encoding="utf-8").read()
        compose_targets = re.findall(r"-\s+\S+:(/[^:\s]+):\w+", compose)
        bad_compose = sorted(t for t in compose_targets
                             if t != image_root and image_root.startswith(t.rstrip("/") + "/"))
        check("compose mounts the document root where the image serves it",
              image_root in compose_targets and not bad_compose,
              f"image serves {image_root}; compose mounts: {compose_targets}")
        print(f"  (image document root: {image_root}, docker run mounts: {len(run_targets)})")

    # --- 5g. The shipped example configuration is valid and honest ----------
    # The deployment instructions copy this file to /etc/dantrix/dantrix.conf;
    # it must validate from the repository root, which is where a reader copies
    # it from. Its header claims it differs from the compiled defaults in
    # exactly three places, so the differences are computed from --print-config
    # rather than trusted -- the header had already gone stale once, pointing at
    # a release that shipped without the feature it named.
    ex = subprocess.run([BIN, "--test-config", "-c", "conf/dantrix.conf"],
                        capture_output=True, timeout=15, cwd=REPO)
    check("the shipped example config validates from the repository root",
          ex.returncode == 0,
          f"exit {ex.returncode}: {ex.stderr.decode('utf-8', 'replace').strip()[:200]}")
    defaults = {}
    for line in out.splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            defaults[key] = value
    example = {}
    for line in open(os.path.join(REPO, "conf", "dantrix.conf"), "r", encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        example[key.strip()] = value.split("#", 1)[0].strip()
    differing = sorted(k for k in example
                       if k in defaults and not config_values_match(example[k], defaults[k]))
    check("the example differs from the compiled defaults exactly where the header says",
          differing == ["access_log", "log_format", "workers"],
          f"header claims workers/access_log/log_format; file differs in: {differing}")

    # --- 5h. The examples table matches the example sources ---------------
    # The table told readers to `curl http://localhost:9000/` for stage 4,
    # which listens on 9003 (9000 belongs to stage 1), and the compile snippet
    # named build/echo_epoll and build/echo_threads, which nothing produces.
    # Both were copied once and never checked against the code.
    ex_dir = os.path.join(REPO, "examples")
    ports = {}
    for name in sorted(os.listdir(ex_dir)):
        if not name.endswith(".c"):
            continue
        src = open(os.path.join(ex_dir, name), "r", encoding="utf-8").read()
        m = re.search(r"^#define PORT (\d+)", src, re.MULTILINE)
        if m:
            ports[name] = int(m.group(1))
    check("every example defines a distinct port", bool(ports) and
          len(set(ports.values())) == len(ports),
          f"ports: {ports}")
    # The table rows name a file and a `curl ...localhost:<port>` command.
    table_bad = []
    for m in re.finditer(r"^\| \d+ \| `examples/([0-9A-Za-z_.-]+\.c)` \|.*?\| `([^`]*)` \|$",
                         readme, re.MULTILINE):
        name, howto = m.group(1), m.group(2)
        pm = re.search(r"localhost:(\d+)", howto)
        if pm and name in ports and int(pm.group(1)) != ports[name]:
            table_bad.append(f"{name}: table says {pm.group(1)}, source says {ports[name]}")
    check("the examples table's port matches the source", not table_bad,
          "; ".join(table_bad))
    # Every `-o build/<name>` in the compile snippet must be what the file
    # actually builds to, which is the source's own basename.
    compile_bad = []
    for m in re.finditer(r"-o build/([A-Za-z0-9_.-]+) examples/([0-9A-Za-z_.-]+\.c)", readme):
        out_name, src_name = m.group(1), m.group(2)
        if out_name != src_name[:-2]:
            compile_bad.append(f"{src_name} -> build/{out_name}")
    check("the compile snippet's output names match the sources", not compile_bad,
          "; ".join(compile_bad))
    print(f"  (example ports: {ports})")

    # --- 5i. The benchmark harness keeps the README's reproducibility promise
    # The README says results are saved in bench/results/<date>-<commit>/
    # together with the hardware and the configuration. No script wrote there:
    # compare_reference.sh only told the reader to do it by hand, and
    # measure.py wrote a single file when asked. The promise is now a flag, and
    # this checks the flag exists and that the results README describes it.
    measure = open(os.path.join(REPO, "bench", "measure.py"), "r", encoding="utf-8").read()
    check("bench/measure.py can save a reproducible record",
          '"--save"' in measure and "results.md" in measure
          and "environment.txt" in measure and "command.txt" in measure,
          "the README promises bench/results/<date>-<commit>/; the harness must write it")
    results_readme = open(os.path.join(REPO, "bench", "results", "README.md"),
                          "r", encoding="utf-8").read()
    check("bench/results/README.md describes the directory format",
          "<date>-<commit>/" in results_readme and "environment.txt" in results_readme,
          results_readme[:120])

    # --- 5j. The documents agree with the release they describe -------------
    # v1.0.0 shipped while the README still said "Configuration keys and HTTP
    # behavior frozen ... Not ready yet" and the roadmap row said the stable
    # release was still planned. A reader would have been told two opposite
    # things by one document. The rule enforced here: once the binary reports a
    # 1.x version, no document may describe the freeze as pending.
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", version_out)
    if m and int(m.group(1)) >= 1:
        pending = []
        for doc in ("README.md", "docs/v1.0-readiness.md"):
            text = open(os.path.join(REPO, doc), "r", encoding="utf-8").read()
            for pat in (r"Not ready yet", r"not ready yet", r"the four decisions",
                        r"four open items", r"still pre-alpha"):
                if re.search(pat, text):
                    pending.append(f"{doc}: {pat}")
        check("no document still describes the v1.0 freeze as pending",
              not pending, "; ".join(pending))
        # The roadmap's v1.0 checklist is the definition of done. At 1.x every
        # box in it must be closed, or the release is claiming a milestone it
        # did not meet.
        start = readme.index("### v1.0.0: Stable")
        nxt = readme.find("\n### ", start + 1)
        section = readme[start:nxt if nxt != -1 else len(readme)]
        open_boxes = re.findall(r"^- \[ \] (.+)$", section, re.MULTILINE)
        check("every v1.0 definition-of-done box is closed at 1.x",
              not open_boxes, f"still open: {open_boxes}")

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
        port = PORT_BASE + 1
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

    # --- 7. The JSON access log is valid JSON with the documented fields ----
    # Accepting `log_format = json` is not enough: the point of the format is
    # that a log pipeline can parse it. Every line must be a JSON object, and
    # the field names are part of the frozen surface, so they are checked by
    # name. The target deliberately contains a quote and a backslash, which are
    # legal request-target bytes here and would break a naive escaper.
    import json as _json

    d = tempfile.mkdtemp(prefix="dantrix-jsonlog-")
    proc = None
    try:
        docroot = os.path.join(d, "root")
        os.makedirs(docroot, exist_ok=True)
        with open(os.path.join(docroot, "a.txt"), "w", encoding="ascii") as f:
            f.write("hi\n")
        port = PORT_BASE + 2
        access = os.path.join(d, "access.log")
        conf = os.path.join(d, "dantrix.conf")
        with open(conf, "w", encoding="ascii") as f:
            f.write(f"bind = 127.0.0.1\nport = {port}\nroot = {docroot}\n"
                    f"access_log = {access}\nlog_format = json\n")
        proc = subprocess.Popen([BIN, "-c", conf], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=REPO)
        for path in ('/a.txt', '/a"b\\c.txt', '/missing'):
            for _ in range(60):
                try:
                    s = socket.create_connection(("127.0.0.1", port), timeout=1)
                except OSError:
                    time.sleep(0.1)
                    continue
                s.sendall(f"GET {path} HTTP/1.1\r\nHost: cfg\r\n"
                          "Connection: close\r\n\r\n".encode())
                while s.recv(65536):
                    pass
                s.close()
                break
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
        proc = None
        lines = [ln for ln in open(access, encoding="utf-8").read().splitlines() if ln]
        parsed, broken = [], []
        for ln in lines:
            try:
                parsed.append(_json.loads(ln))
            except ValueError as e:
                broken.append(f"{e}: {ln[:80]}")
        check("every JSON access-log line parses", bool(lines) and not broken,
              f"{len(broken)} invalid: {broken[:2]}")
        want = {"ts", "peer", "method", "target", "protocol", "status", "bytes",
                "referer", "user_agent"}
        got = set(parsed[0]) if parsed else set()
        check("the JSON log has exactly the documented field names", got == want,
              f"missing: {sorted(want - got)}, unexpected: {sorted(got - want)}")
        check("the JSON log records the quoted/backslash target intact",
              any(o.get("target") == '/a"b\\c.txt' for o in parsed),
              f"targets: {[o.get('target') for o in parsed]}")
        check("the JSON log records the 404", any(o.get("status") == 404 for o in parsed),
              f"statuses: {[o.get('status') for o in parsed]}")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        shutil.rmtree(d, ignore_errors=True)

    print(f"config test: {'FAILED' if failures else 'PASSED'} ({checks} checks)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
