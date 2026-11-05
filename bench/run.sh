#!/usr/bin/env bash
# 'make bench' entry: stock vs balanced vs max on the offline workload (A),
# alternating runs (ABAB), then the summary table. Takes ~10 minutes.
#   bench/run.sh [A|B] [PAIRS]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-A} P=${2:-3}
make -s -C "$HERE"
TAG=${TAG:-make-bench-$(date +%s)}
export TAG
"$HERE/bench.sh" ab stock balanced "$W" "$P"
"$HERE/bench.sh" run max "$W" "$P"
"$HERE/report.sh" "$TAG"
