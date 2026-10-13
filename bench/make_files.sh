#!/usr/bin/env bash
# Create the benchmark payload files under bench/www.
#
# Sizes match the README scenarios: 1k, 10k, 100k, 1m, 100m.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
WWW="$DIR/www"
mkdir -p "$WWW"

for size in 1K 10K 100K 1M 100M; do
    name="$(echo "$size" | tr 'A-Z' 'a-z').bin"
    # dd is faster and does not need to keep the whole file in memory.
    case "$size" in
        1K)   bytes=$((1024)) ;;
        10K)  bytes=$((10 * 1024)) ;;
        100K) bytes=$((100 * 1024)) ;;
        1M)   bytes=$((1024 * 1024)) ;;
        100M) bytes=$((100 * 1024 * 1024)) ;;
    esac
    if [ ! -f "$WWW/$name" ] || [ "$(stat -c %s "$WWW/$name")" != "$bytes" ]; then
        head -c "$bytes" /dev/urandom > "$WWW/$name"
        echo "created $WWW/$name ($bytes bytes)"
    else
        echo "kept    $WWW/$name ($bytes bytes)"
    fi
done

# A small index so `GET /` works without a listing.
if [ ! -f "$WWW/index.html" ]; then
    printf '<h1>Dantrix bench</h1>\n' > "$WWW/index.html"
    echo "created $WWW/index.html"
fi

echo "bench payload ready in $WWW"
