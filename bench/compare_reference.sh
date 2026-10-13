#!/usr/bin/env bash
# Compare Dantrix against a reference instance of any static file server
# running on a neighbouring port.
#
# Usage:
#   bench/compare_reference.sh [DANTRIX_PORT] [REFERENCE_PORT] [DURATION]
#
# Both servers must already be running with equivalent configurations:
#   * Dantrix: ./bin/dantrix -r bench/www -p 8080 -w 4 --access-log off
#   * Reference: whatever serves the same payload on REFERENCE_PORT (8081)
#
# The script runs the same wrk scenario against both and prints a small table.
# It never starts or stops the servers; that keeps the comparison honest (no
# start-up order effects) and avoids clobbering a running service.
set -uo pipefail

DPORT="${1:-8080}"
NPORT="${2:-8081}"
DURATION="${3:-30s}"
WRK="${WRK:-wrk}"
DIR="$(cd "$(dirname "$0")" && pwd)"

command -v "$WRK" >/dev/null 2>&1 || {
    echo "compare_reference.sh: '$WRK' not found; install wrk first" >&2
    exit 1
}

measure() {
    local label="$1" url="$2" scenario="$3"
    local out rps
    case "$scenario" in
        s1) out="$("$WRK" -t4 -c256 -d"$DURATION" "$url/1k.bin" 2>&1)" ;;
        s3) out="$("$WRK" -t4 -c64 -d"$DURATION" "$url/100m.bin" 2>&1)" ;;
        *)  out="$("$WRK" -t4 -c256 -d"$DURATION" "$url/1k.bin" 2>&1)" ;;
    esac
    rps="$(printf '%s\n' "$out" | awk '/Requests\/sec:/ {print $2; exit}')"
    printf '%-10s %-8s %s req/s\n' "$label" "$scenario" "${rps:-n/a}"
}

echo "Dantrix vs reference ($DURATION per scenario)"
echo "  Dantrix: http://127.0.0.1:$DPORT"
echo "  Reference: http://127.0.0.1:$NPORT"
echo
printf '%-10s %-8s %s\n' "server" "scenario" "result"
for scenario in s1 s3; do
    measure "Dantrix" "http://127.0.0.1:$DPORT" "$scenario"
    measure "Reference" "http://127.0.0.1:$NPORT" "$scenario"
done
echo
echo "Record the full output in bench/results/ together with the hardware,"
echo "kernel version, compiler flags and commit hash (see the README)."
