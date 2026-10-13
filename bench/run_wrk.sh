#!/usr/bin/env bash
# Run every benchmark scenario from the README against a Dantrix instance.
#
# Usage:
#   bench/run_wrk.sh [URL] [WRK] [DURATION]
#
#   URL       base URL, default http://127.0.0.1:8080
#   WRK       wrk binary, default `wrk`
#   DURATION  per-scenario duration, default 30s
#
# Requires: wrk (https://github.com/wg/wrk) and, for S6, wrk2.
set -uo pipefail

URL="${1:-http://127.0.0.1:8080}"
WRK="${2:-wrk}"
DURATION="${3:-30s}"
DIR="$(cd "$(dirname "$0")" && pwd)"

command -v "$WRK" >/dev/null 2>&1 || {
    echo "run_wrk.sh: '$WRK' not found; install wrk first" >&2
    exit 1
}

run() {
    local id="$1" desc="$2"
    shift 2
    echo
    echo "=== $id: $desc"
    echo "    $*"
    "$@" || echo "    (scenario $id failed)"
}

run S1 "small file, keep-alive" \
    "$WRK" -t4 -c256 -d"$DURATION" --latency "$URL/1k.bin"
run S2 "without keep-alive" \
    "$WRK" -t4 -c256 -d"$DURATION" -H "Connection: close" "$URL/1k.bin"
run S3 "large file (sendfile)" \
    "$WRK" -t4 -c64 -d"$DURATION" "$URL/100m.bin"
run S4 "high concurrency" \
    "$WRK" -t4 -c10000 -d"$DURATION" "$URL/1k.bin"
run S7 "mixed file sizes" \
    "$WRK" -t4 -c256 -d"$DURATION" -s "$DIR/mixed.lua" "$URL"

if command -v wrk2 >/dev/null 2>&1; then
    run S6 "latency at a fixed rate" \
        wrk2 -t2 -c100 -d"$DURATION" -R20000 --latency "$URL/1k.bin"
else
    echo
    echo "=== S6 skipped: wrk2 not installed"
fi

echo
echo "run_wrk.sh: done (S5 idle and S8 slowloris are separate tools)"
