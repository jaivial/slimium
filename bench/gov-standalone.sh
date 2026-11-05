#!/usr/bin/env bash
# bench/gov-standalone.sh -- steady iso on workload A (bench/www, 6 tabs on 6 loopback hosts):
# 'slimium start' (profile + governor, the way users and cu get it) vs the stock ELF with no flags.
#   bench/gov-standalone.sh [PAIRS]        ABAB, default 2 pairs; STEADY=150 s of 5 s samples
# Prints per run: settled iso, steady min/median/max/time-weighted mean (MB, crashpad included as
# in bench.sh). Ports: CDP 9450 (PORT), HTTP 9451 (HTTP_PORT).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export PORT=${PORT:-9450} HTTP_PORT=${HTTP_PORT:-9451}
BENCH_LIB=1 . "$HERE/bench.sh"
SLIM=${SLIM:-$HERE/../slimium} PAIRS=${1:-2} STEADY=${STEADY:-150} PROF=${SLIM_PROFILE:-balanced}
export SLIM_STATE_DIR=$(mktemp -d /tmp/slim-gov-sa.XXXXXX)
trap 'stop_slim; cleanup; rm -rf "${SLIM_STATE_DIR:?}"' EXIT
start_slim() {
  UDD=$(mktemp -d /tmp/slim-bench-XXXXXX)
  BPID=$("$SLIM" --session sa --json start --port "$PORT" --user-data-dir "$UDD" --profile "$PROF" | jq -r .pid)
  SLIMRUN=1
}
stop_slim() { [ -n "${SLIMRUN:-}" ] || return 0; "$SLIM" --session sa stop >/dev/null 2>&1; rm -rf "$UDD"; BPID= UDD= SLIMRUN=; }
steady() { # -> "settled min median max twmean" in MB
  local t0 i
  t0=$(date +%s.%N)
  for i in $(seq 0 5 "$STEADY"); do
    echo "$(echo "$(date +%s.%N) - $t0" | bc) $("$BIN/slim-memstat" --json --crashpad "$BPID" | jq .iso_kb)"
    sleep 4.7
  done | python3 -c '
import sys
r=[tuple(map(float,l.split())) for l in sys.stdin if l.strip()]
v=sorted(x[1] for x in r); n=len(v)
tw=sum((r[i+1][0]-r[i][0])*(r[i][1]+r[i+1][1])/2 for i in range(n-1))/(r[-1][0]-r[0][0])
print("settled %.1f steady %.1f %.1f %.1f %.1f" % (r[0][1]/1024, v[0]/1024, v[n//2]/1024, v[-1]/1024, tw/1024))'
}
echo "# gov-standalone $(date -Is): workload A, settle ${SETTLE}s, steady ${STEADY}s; slimium profile $PROF + governor vs stock ELF (no flags)"
for p in $(seq 1 "$PAIRS"); do
  start_slim; open_workload A; echo "slimium $p $(steady)"; stop_slim
  launch stock || exit 1; open_workload A; echo "stock   $p $(steady)"; stop
done
