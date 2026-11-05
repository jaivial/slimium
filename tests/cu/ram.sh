#!/usr/bin/env bash
# tests/cu/ram.sh -- browser RAM through cu: the same scripted cu flow on
# slimium and on stock Chromium, PSS summed over the whole browser process tree.
#
# Flow per run (fresh temp profile, isolated cu-slim daemon on HTTP 8794 / CDP 9394):
#   10 leased tabs (cu --new-tab navigate) on the local fixtures + 3 real sites
#   (example.com, en.wikipedia.org, news.ycombinator.com), cu snapshot in each,
#   settle SETTLE s, then PSS of the browser tree (median of 3 samples 1 s apart).
# Median of RUNS runs per config. Configs (CONFIGS, space separated):
#   slim          cu-slim on slimium-chrome (real wrapper if installed, else stand-in), SLIM_PROFILE=balanced
#   slim-max      same, SLIM_PROFILE=max
#   stock-script  /usr/bin/chromium-browser (Fedora's wrapper script: how cu runs it by default)
#   stock-elf     /usr/lib64/chromium-browser/chromium-browser, cu fast mode (cu's own flags only)
#   stock-compat  stock ELF, CU_MODE=compat (cu passes only 3 flags: closest to plain Chromium)
#   (all with CU_HEADLESS=1, as cu-slim runs)
#
#   tests/cu/ram.sh                         # all configs, 3 runs
#   RUNS=1 CONFIGS="slim stock-elf" tests/cu/ram.sh
set -u
cd "$(dirname "$0")" || exit 2
source ./lib.sh
RUNS=${RUNS:-3}
SETTLE=${SETTLE:-5}
CONFIGS=${CONFIGS:-slim slim-max stock-script stock-elf stock-compat}
REAL=${REAL-https://example.com https://en.wikipedia.org/wiki/Web_browser https://news.ycombinator.com}
FIX_PORT=${FIX_PORT:-8791} HTTP_PORT=${HTTP_PORT:-8794} CDP_PORT=${CDP_PORT:-9394}
BASE=http://127.0.0.1:$FIX_PORT
OUT=${OUT:-$CU_TESTS_DIR/results/ram-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
ELF=/usr/lib64/chromium-browser/chromium-browser
H=

cleanup() { stop_instance R; stop_fixtures; [ -n "$H" ] && rm -rf "${H:?}"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

config_env() { # sets B (browser), MODE, PROF for a config name
  MODE=fast PROF=balanced
  case $1 in
    slim) B=$(slim_browser) ;;
    slim-max) B=$(slim_browser) PROF=max ;;
    custom) B=$RAM_BROWSER ;;  # any executable, cu fast mode
    slim-env) B=$(slim_browser) PROF=${SLIM_PROFILE_ENV:-balanced} ;;  # + SLIM_EXTRA/SLIM_OVERRIDE from env (stand-in)
    stock-script) B=/usr/bin/chromium-browser ;;
    stock-elf) B=$ELF ;;
    stock-compat) B=$ELF MODE=compat ;;
    *) echo "unknown config $1" >&2; return 1 ;;
  esac
}

one_run() { # one_run CONFIG -> prints "pss_kb rss_kb procs tabs_ok"
  local cfg=$1 i url err tab okc=0 fx s
  config_env "$cfg" || return 1
  H=$(mktemp -d /tmp/cu-slim-ram.XXXXXX)
  export CU_SLIM_HOME=$H CU_SLIM_MODE=$MODE SLIM_PROFILE=$PROF
  start_instance R "$H" "$HTTP_PORT" "$CDP_PORT" "$B" >&2 || return 1
  local urls=()
  for fx in form login links iframe shadow long download late upload index; do urls+=("$BASE/$fx.html"); done
  for url in $REAL; do urls+=("$url"); done
  for url in "${urls[@]}"; do
    err=$H/nav.err
    "$CU_SLIM" --new-tab navigate "$url" >/dev/null 2>"$err"
    tab=$(grep -o 'fresh tab ([0-9A-F]*)' "$err" | tr -dc '0-9A-F')
    [ -n "$tab" ] || continue
    s=$("$CU_SLIM" snapshot --tab "$tab" 2>/dev/null)
    case $s in *'ref=e'*|*heading*) okc=$((okc+1)) ;; esac
  done
  sleep "$SETTLE"
  local m1 m2 m3
  m1=$(tree_mem "$INST_BROWSER_R"); sleep 1; m2=$(tree_mem "$INST_BROWSER_R"); sleep 1; m3=$(tree_mem "$INST_BROWSER_R")
  printf '%s\n%s\n%s\n' "$m1" "$m2" "$m3" | sort -n | sed -n 2p | tr -d '\n'
  echo " $okc/${#urls[@]}"
  stop_instance R
  rm -rf "${H:?}"; H=
}

before=$(user_cu_fingerprint)
start_fixtures "$FIX_PORT" || exit 1
{
  echo "# cu RAM comparison $(date -Is) on $(hostname); runs=$RUNS settle=${SETTLE}s"
  echo "# slim browser: $(slim_browser); cu: $("$HOME/.local/bin/cu-slim-bin" help >/dev/null 2>&1; git -C "$HOME/cu-wt/slimium" log --oneline -1 2>/dev/null)"
  echo "# flow: 10 fixture tabs + $(echo $REAL | wc -w) real sites, one leased tab each, snapshot each"
  echo "# config run pss_kb rss_kb procs snapshots_ok"
} | tee "$OUT"
declare -A med=()
for cfg in $CONFIGS; do
  vals=()
  for r in $(seq 1 "$RUNS"); do
    line=$(one_run "$cfg") || { echo "$cfg run $r failed" | tee -a "$OUT"; continue; }
    echo "$cfg $r $line" | tee -a "$OUT"
    vals+=("${line%% *}")
  done
  [ ${#vals[@]} -gt 0 ] && med[$cfg]=$(printf '%s\n' "${vals[@]}" | median)
done
{
  echo "# median PSS (MB) of $RUNS runs:"
  for cfg in $CONFIGS; do [ -n "${med[$cfg]:-}" ] && printf '#   %-13s %7.1f MB\n' "$cfg" "$(echo "${med[$cfg]} / 1024" | bc -l)"; done
  for s in slim slim-max; do
    [ -n "${med[$s]:-}" ] || continue
    for b in stock-elf stock-script stock-compat; do
      [ -n "${med[$b]:-}" ] && printf '#   ratio %-9s / %-13s = %.3f\n' "$s" "$b" "$(echo "${med[$s]} / ${med[$b]}" | bc -l)"
    done
  done
} | tee -a "$OUT"
after=$(user_cu_fingerprint)
if [ "$before" = "$after" ]; then echo "# user's cu untouched" | tee -a "$OUT"; else echo "# WARNING user's cu changed: $before | $after" | tee -a "$OUT"; fi
