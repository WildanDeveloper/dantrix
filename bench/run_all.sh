#!/usr/bin/env bash
# Run every benchmark scenario in one go (and prepare the payloads first).
#
# Requires: wrk, python3. wrk2 is optional (scenario S6).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== preparing benchmark payloads"
"$DIR/make_files.sh"

echo
echo "=== building the idle-connection tool"
mkdir -p "$DIR/../build"
gcc -O2 -Wall -Wextra -o "$DIR/../build/idle_conns" "$DIR/tools/idle_conns.c"
echo "build/idle_conns ready"

echo
echo "=== S8: slowloris resilience"
# Uses a short delay so the run does not take minutes.
python3 "$DIR/slowloris.py" --host 127.0.0.1 --port "${BENCH_PORT:-8080}" \
    --count 200 --delay 0.2 --duration 5 || true

echo
echo "=== S1..S7: wrk scenarios"
"$DIR/run_wrk.sh" "${BENCH_URL:-http://127.0.0.1:${BENCH_PORT:-8080}}" \
    "${BENCH_WRK:-wrk}" "${BENCH_DURATION:-30s}"

echo
echo "=== S5: idle connections (memory per connection)"
"$DIR/../build/idle_conns" -h 127.0.0.1 -p "${BENCH_PORT:-8080}" -n 10000 -t 10 -r -q || true

echo
echo "bench: all scenarios done"
