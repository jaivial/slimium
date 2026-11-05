#!/usr/bin/env bash
# tests/cu/gov-ram.sh -- isolated footprint (iso, src/mem.c) through cu-slim, slimium (with its
# memory governor) vs stock Chromium, on the cu flow of ram.sh:
#   10 leased tabs on the fixtures + 3 real sites, cu snapshot in each, then
#   settled   iso SETTLE s after the flow (before the governor's first idle pass on slimium)
#   steady    iso every 5 s for STEADY s (min / median / max / time-weighted mean)
#   active    peak iso (1 s samples) during an active pass: snapshot + shot of every tab
#   after     iso 30 s after the active pass
# iso = sum Pss_Anon + Pss_Shmem + union of resident file pages of the browser tree (crashpad not
# included). Configs: slim (cu-slim + ~/.local/bin/slimium-chrome, SLIM_PROFILE as given, default
# balanced; HTTP 8790 / CDP 9390) and stock (CU_BROWSER=/usr/lib64/chromium-browser/chromium-browser,
# HTTP 8791 / CDP 9391). Temp daemon dirs; fixtures on 8796. Only PIDs started here are signalled.
#   tests/cu/gov-ram.sh                 # CONFIGS="slim stock" RUNS=1 STEADY=150
set -u
cd "$(dirname "$0")" || exit 2
source ./lib.sh
CONFIGS=${CONFIGS:-slim stock} RUNS=${RUNS:-1} SETTLE=${SETTLE:-4} STEADY=${STEADY:-150}
REAL=${REAL-https://example.com https://en.wikipedia.org/wiki/Web_browser https://news.ycombinator.com}
FIX_PORT=${FIX_PORT:-8796}
BASE=http://127.0.0.1:$FIX_PORT
MEMSTAT=${MEMSTAT:-$CU_TESTS_DIR/../../build/bench/slim-memstat}
OUT=${OUT:-$CU_TESTS_DIR/results/gov-ram-$(date +%Y%m%d-%H%M%S).txt}
ELF=/usr/lib64/chromium-browser/chromium-browser
[ -x "$MEMSTAT" ] || { echo "build the bench tools first: make -C bench" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
H= SP=
cleanup() { [ -n "$SP" ] && kill "$SP" 2>/dev/null; stop_instance R; stop_fixtures; [ -n "$H" ] && rm -rf "${H:?}"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

iso() { "$MEMSTAT" --json "$1" 2>/dev/null | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["iso_kb"], d["anon_kb"]+d["shmem_kb"], d["file_union_kb"], d["procs"])'; }
stats() { # lines "t iso" -> min median max twmean (MB)
  python3 -c '
import sys
r=[tuple(map(float,l.split()[:2])) for l in sys.stdin if l.strip()]
v=sorted(x[1] for x in r); n=len(v)
med=v[n//2] if n%2 else (v[n//2-1]+v[n//2])/2
tw=sum((r[i+1][0]-r[i][0])*(r[i][1]+r[i+1][1])/2 for i in range(n-1))/(r[-1][0]-r[0][0]) if n>1 else v[0]
print("%.1f %.1f %.1f %.1f" % (v[0]/1024, med/1024, v[-1]/1024, tw/1024))'; }

one_run() { # one_run CONFIG RUN
  local cfg=$1 run=$2 B port cdp url err tab okc=0 shots=0 fx t0 i
  local mode=fast hib=0
  case $cfg in
    slim) B=$(slim_browser) port=8790 cdp=9390 ;;
    slim-hib) B=$(slim_browser) port=8790 cdp=9390 hib=3 ;;   # + opt-in hibernation, K=3, untouched >= 20 s
    stock) B=$ELF port=8791 cdp=9391 ;;
    stock-compat) B=$ELF port=8791 cdp=9391 mode=compat ;;   # cu passes only 3 flags: plain Chromium
    *) echo "unknown config $cfg" >&2; return 1 ;;
  esac
  H=$(mktemp -d /tmp/cu-gov-ram.XXXXXX)
  export CU_SLIM_HOME=$H CU_SLIM_MODE=$mode SLIM_PROFILE=${SLIM_PROFILE:-balanced} SLIM_GOV_LOG=$H/gov.log
  export SLIM_GOV_HIBERNATE=$hib SLIM_GOV_HIBERNATE_MIN_S=20
  start_instance R "$H" "$port" "$cdp" "$B" >&2 || return 1
  local P=$INST_BROWSER_R tabs=()
  local urls=()
  for fx in form login links iframe shadow long download late upload index; do urls+=("$BASE/$fx.html"); done
  for url in $REAL; do urls+=("$url"); done
  for url in "${urls[@]}"; do
    err=$H/nav.err
    "$CU_SLIM" --new-tab navigate "$url" >/dev/null 2>"$err"
    tab=$(grep -o 'fresh tab ([0-9A-F]*)' "$err" | tr -dc '0-9A-F')
    [ -n "$tab" ] || continue
    tabs+=("$tab")
    case $("$CU_SLIM" snapshot --tab "$tab" 2>/dev/null) in *'ref=e'*|*heading*) okc=$((okc+1)) ;; esac
  done
  sleep "$SETTLE"
  local settled; settled=$(iso "$P")
  t0=$(date +%s.%N)
  : >"$H/steady"
  for i in $(seq 0 5 "$STEADY"); do
    echo "$(echo "$(date +%s.%N) - $t0" | bc) $(iso "$P")" >>"$H/steady"
    sleep 4.7
  done
  # active pass, sampled every second in the background
  ( while :; do echo "$(date +%s.%N) $(iso "$P")"; sleep 1; done ) >"$H/active" & SP=$!
  local asnap=0
  for tab in "${tabs[@]}"; do
    case $("$CU_SLIM" snapshot --tab "$tab" 2>/dev/null) in *'ref=e'*|*heading*) asnap=$((asnap+1)) ;; esac
    "$CU_SLIM" shot "$H/s-$tab.png" --tab "$tab" >/dev/null 2>&1 && [ -s "$H/s-$tab.png" ] && shots=$((shots+1))
  done
  "$CU_SLIM" --new-tab navigate "$BASE/long.html" >/dev/null 2>&1
  kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; SP=
  sleep 30
  local after; after=$(iso "$P")
  local st ac gcpu=- npass=- nrec=-
  st=$(stats <"$H/steady")
  ac=$(awk '{print $1, $2}' "$H/active" | stats | awk '{print $3}')
  if [ "$cfg" != stock ] && [ "$cfg" != stock-compat ]; then
    local G; G=$(pgrep -f "^slimium gov --daemon --pid $P ")
    [ -n "$G" ] && gcpu=$(ps -o time= -p "$G" | tr -d ' ')
    npass=$(grep -c '"ev":"pass".*"gc":1' "$H/gov.log" 2>/dev/null) nrec=$(grep -c '"ev":"pass".*"reclaim":[12]' "$H/gov.log" 2>/dev/null)
    nrec="$nrec hibernated $(grep -c '"ev":"hibernate"' "$H/gov.log") restored $(grep -c '"ev":"restore"' "$H/gov.log")"
    cp "$H/gov.log" "${OUT%.txt}-$cfg-$run.gov.log" 2>/dev/null
  fi
  cp "$H/steady" "${OUT%.txt}-$cfg-$run.steady" 2>/dev/null
  printf '%s %s settled_mb %.1f (anon+shmem %.1f file_union %.1f procs %s) steady_min/med/max/twmean %s active_peak %s after30 %.1f snapshots %d/%d active_snapshots %d shots %d gc_passes %s reclaims %s gov_cpu %s\n' \
    "$cfg" "$run" "$(echo "${settled%% *} / 1024" | bc -l)" "$(echo "$settled" | awk '{print $2/1024}')" "$(echo "$settled" | awk '{print $3/1024}')" \
    "$(echo "$settled" | awk '{print $4}')" "$st" "$ac" "$(echo "${after%% *} / 1024" | bc -l)" "$okc" "${#urls[@]}" "$asnap" "$shots" "$npass" "$nrec" "$gcpu"
  stop_instance R
  rm -rf "${H:?}"; H=
}

before=$(user_cu_fingerprint)
start_fixtures "$FIX_PORT" || exit 1
{ echo "# gov-ram $(date -Is) on $(hostname); settle=${SETTLE}s steady=${STEADY}s (5 s samples); SLIM_PROFILE=${SLIM_PROFILE:-balanced}"
  echo "# slim browser: $(slim_browser) ($(git -C "$CU_TESTS_DIR" log --oneline -1 2>/dev/null)); MB = iso (anon+shmem+file union) of the browser tree"; } | tee "$OUT"
for cfg in $CONFIGS; do for r in $(seq 1 "$RUNS"); do one_run "$cfg" "$r" | tee -a "$OUT"; done; done
after=$(user_cu_fingerprint)
if [ "$before" = "$after" ]; then echo "# user's cu untouched" | tee -a "$OUT"; else echo "# WARNING user's cu changed: $before | $after" | tee -a "$OUT"; fi
