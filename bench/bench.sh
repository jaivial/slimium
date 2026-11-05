#!/usr/bin/env bash
# slimium RAM benchmark: same engine, same workload, different flag sets.
#
#   bench.sh run   CONFIG WORKLOAD [REPS]   measure; appends JSON lines to results/raw.jsonl
#   bench.sh ab    CFG_A CFG_B WORKLOAD [PAIRS]  alternate A,B,A,B... (cancels drift/noise)
#   bench.sh smoke CONFIG                   functional smoke check; appends to results/smoke.jsonl
#   bench.sh args  CONFIG                   print the argv a config launches
#
# CONFIG is a file in configs/ (or a path): one argument per line, '#' comments.
# A line "@merge LEVEL" runs the config's argv through slim_profile_merge_argv
# (src/profile.c) the way slimium-chrome does; "@engine PATH" overrides ENGINE.
# WORKLOAD is A (offline pages served by slim-httpd) or B (real web sites).
# Env: ENGINE (default the Chromium ELF), PORT (9330), HTTP_PORT (9340),
#      SETTLE (8 s after loads), SAMPLES (3), TAG (free text stored in results).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/../build/bench
RES=$HERE/results
ENGINE=${ENGINE:-/usr/lib64/chromium-browser/chromium-browser}
PORT=${PORT:-9330}
HTTP_PORT=${HTTP_PORT:-9340}
SETTLE=${SETTLE:-8}
SAMPLES=${SAMPLES:-3}
TAG=${TAG:-}
HOSTS="127.0.0.1 127.0.0.2 127.0.0.3 127.0.0.4 127.0.0.5 127.0.0.6 127.0.0.7"
mkdir -p "$RES"

die() { echo "bench: $*" >&2; exit 1; }
[ -x "$BIN/slim-memstat" ] || make -s -C "$HERE" || die "build failed"

H=http://127.0.0.1:$HTTP_PORT
url_A() {
  local p=$HTTP_PORT
  echo "http://127.0.0.1:$p/text.html http://127.0.0.2:$p/spa.html http://127.0.0.3:$p/canvas.html \
http://127.0.0.4:$p/frames.html http://127.0.0.5:$p/list.html http://127.0.0.6:$p/form.html"
}
titles_A="Bench Text Article|Bench SPA Dashboard|Bench Canvas Gallery|Bench Frames Portal|Bench Long List|Bench Checkout Form"
url_B() {
  echo "https://en.wikipedia.org/wiki/Linux https://github.com/torvalds/linux https://www.bbc.com/news \
https://www.youtube.com/ https://developer.mozilla.org/en-US/docs/Web/JavaScript https://www.ebay.com/b/Laptops-Netbooks/175672/bn_1648276"
}

HTTPD_PID=
start_httpd() {
  curl -s -o /dev/null --max-time 1 "$H/index.html" && return 0
  "$BIN/slim-httpd" "$HERE/www" "$HTTP_PORT" $HOSTS >/dev/null 2>&1 &
  HTTPD_PID=$!
  for _ in $(seq 50); do curl -s -o /dev/null --max-time 1 "$H/index.html" && return 0; sleep 0.1; done
  die "slim-httpd did not start"
}

CFG_ENGINE=
config_args() {   # $1 config, $2 user-data-dir -> argv lines on stdout
  local f=$1 merge="" line
  [ -f "$f" ] || f=$HERE/configs/$1
  [ -f "$f" ] || die "no config $1"
  CFG_ENGINE=$ENGINE
  local -a flags=()
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|'#'*) ;;
      '@merge '*) merge=${line#@merge } ;;
      '@engine '*) CFG_ENGINE=${line#@engine } ;;
      *) flags+=("$line") ;;
    esac
  done < "$f"
  # Shape of cu's own launch (policy.rs args()): port, flags, profile, headless, url.
  local -a argv=("--remote-debugging-port=$PORT" "${flags[@]}" "--user-data-dir=$2")
  local h=1; for a in "${flags[@]}"; do case "$a" in --headless*) h=0;; esac; done
  [ $h = 1 ] && argv+=("--headless=new")
  argv+=("about:blank")
  if [ -n "$merge" ]; then "$BIN/slim-merge" "$merge" "${argv[@]}"; else printf '%s\n' "${argv[@]}"; fi
}

BPID= UDD=
launch() {   # $1 config
  UDD=$(mktemp -d /tmp/slim-bench-XXXXXX)
  local -a argv; mapfile -t argv < <(config_args "$1" "$UDD")
  config_args "$1" "$UDD" >/dev/null   # sets CFG_ENGINE in this shell
  setsid "$CFG_ENGINE" "${argv[@]}" >"$UDD.log" 2>&1 &
  BPID=$!
  for _ in $(seq 150); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/json/version" && return 0
    kill -0 $BPID 2>/dev/null || break
    sleep 0.1
  done
  echo "launch failed; log:" >&2; tail -5 "$UDD.log" >&2; stop; return 1
}

stop() {
  [ -n "$BPID" ] || return 0
  kill -TERM -- -$BPID 2>/dev/null
  for _ in $(seq 50); do kill -0 $BPID 2>/dev/null || break; sleep 0.1; done
  kill -KILL -- -$BPID 2>/dev/null
  sleep 0.3
  rm -rf "$UDD" "$UDD.log" "$UDD.err"; BPID= UDD=
}
cleanup() { stop; [ -n "$HTTPD_PID" ] && kill $HTTPD_PID 2>/dev/null; }
trap cleanup EXIT
trap 'exit 130' INT TERM

page_ws() {   # ws URLs of page targets, one per line
  curl -s "http://127.0.0.1:$PORT/json/list" | jq -r '.[] | select(.type=="page") | .webSocketDebuggerUrl'
}
browser_ws() { curl -s "http://127.0.0.1:$PORT/json/version" | jq -r .webSocketDebuggerUrl; }
eval_ws() {   # $1 ws, $2 expression -> value (string/number) or empty
  "$BIN/slim-cdp" -t 5 "$1" "Runtime.evaluate {\"expression\":$(jq -Rn --arg e "$2" '$e'),\"returnByValue\":true}" 2>/dev/null |
    jq -r '.result.result.value // empty' 2>/dev/null
}

open_workload() {   # $1 A|B ; opens every URL as a tab, waits for loads, then SETTLE
  local w=$1 urls limit=20
  [ "$w" = A ] && { start_httpd; urls=$(url_A); } || { urls=$(url_B); limit=45; }
  for u in $urls; do curl -s -o /dev/null -X PUT "http://127.0.0.1:$PORT/json/new?$u"; done
  # Loaded = every opened URL has left about:blank and is readyState complete
  # (a tab whose navigation has not committed yet still shows a complete
  # about:blank document).
  local t0=$SECONDS ws ok want; want=$(echo $urls | wc -w)
  while [ $((SECONDS - t0)) -lt $limit ]; do
    ok=0
    for ws in $(page_ws); do
      [ "$(eval_ws "$ws" 'location.href=="about:blank"?"blank":document.readyState')" = complete ] && ok=$((ok+1))
    done
    [ $ok -ge $want ] && break
    sleep 1
  done
  LOAD_S=$((SECONDS - t0))
  sleep "$SETTLE"
}

# Metric: iso_kb (isolated footprint = Pss_Anon + Pss_Shmem + union of resident
# file pages, src/mem.c). PSS alone is distorted by the other Chromium
# processes on the machine that share our file pages.
sample() {   # -> one JSON object of the median-iso sample, with all samples
  local -a s=()
  for i in $(seq "$SAMPLES"); do
    s+=("$("$BIN/slim-memstat" --json --crashpad $BPID)")
    [ "$i" -lt "$SAMPLES" ] && sleep 2
  done
  printf '%s\n' "${s[@]}" | jq -sc 'sort_by(.iso_kb) as $s | $s[(($s|length)/2|floor)] + {samples: [ $s[].iso_kb ]}'
}

titles_json() {
  curl -s "http://127.0.0.1:$PORT/json/list" | jq -c '[.[] | select(.type=="page") | .title]'
}

cmd_run() {
  local cfg=$1 w=$2 reps=${3:-3} r0=${REP0:-0}
  for r in $(seq $((r0 + 1)) $((r0 + reps))); do
    launch "$cfg" || { echo "{\"config\":\"$cfg\",\"workload\":\"$w\",\"error\":\"launch\"}" >> "$RES/raw.jsonl"; continue; }
    open_workload "$w"
    local m titles breakdown
    m=$(sample)
    titles=$(titles_json)
    breakdown=$("$BIN/slim-memstat" --json -v --crashpad $BPID | jq -sc '[.[] | select(.type)] | group_by(.type) | map({type: .[0].type, n: length, iso_kb: (map(.iso_kb) | add), anon_kb: (map(.anon_kb) | add), pss_kb: (map(.pss_kb) | add)})')
    stop
    local line
    line=$(jq -cn --arg c "$(basename "$cfg")" --arg w "$w" --arg e "$CFG_ENGINE" --arg tag "$TAG" --argjson r "$r" \
      --argjson m "$m" --argjson t "$titles" --argjson b "$breakdown" --argjson load "$LOAD_S" \
      '{config:$c, workload:$w, rep:$r, engine:$e, tag:$tag, date:(now|todate), load_s:$load} + $m + {titles:$t, by_type:$b}')
    echo "$line" >> "$RES/raw.jsonl"
    echo "$line" | jq -r '"\(.config) \(.workload)#\(.rep): iso \(.iso_kb/1024|floor) MB anon \(.anon_kb/1024|floor) pss \(.pss_kb/1024|floor) procs \(.procs) tabs \(.titles|length) load \(.load_s)s"'
  done
}

# ---- functional smoke check ------------------------------------------------
cmd_smoke() {
  local cfg=$1 fails="" out v
  launch "$cfg" || { echo "{\"config\":\"$cfg\",\"smoke\":\"launch-failed\"}" >> "$RES/smoke.jsonl"; return 1; }
  SETTLE=2 open_workload A
  local -a ws; mapfile -t ws < <(page_ws)
  local B; B=$(browser_ws)
  # 1. every tab listed with its title
  local t; t=$(titles_json)
  local IFS_OLD=$IFS; IFS='|'; for want in $titles_A; do echo "$t" | grep -qF "$want" || fails+="title($want) "; done; IFS=$IFS_OLD
  # 2. Runtime.evaluate on every page
  for w in "${ws[@]}"; do [ "$(eval_ws "$w" '1+1')" = 2 ] || fails+="eval "; done
  # 3. screenshot of the canvas tab is not blank
  local cws; cws=$(curl -s "http://127.0.0.1:$PORT/json/list" | jq -r '.[] | select(.title=="Bench Canvas Gallery") | .webSocketDebuggerUrl' | head -1)
  v=$("$BIN/slim-cdp" -m 0 -t 30 "$cws" 'Page.bringToFront' 'Page.captureScreenshot {"format":"png"}' 2>"$UDD.err" | tail -1 | jq -r '.result.data | length')
  [ "${v:-0}" -gt 20000 ] || { echo "screenshot failed: $(head -c 300 "$UDD.err")" >&2; }
  [ "${v:-0}" -gt 20000 ] || fails+="screenshot($v) "
  SHOT_LEN=$v
  # 4+5. isolated browser context (and one with a proxy), cross-site navigation in it
  out=$("$BIN/slim-cdp" -t 20 "$B" 'Target.createBrowserContext {"disposeOnDetach":true}' \
      "Target.createTarget {\"url\":\"http://127.0.0.1:$HTTP_PORT/text.html\",\"browserContextId\":\"\$browserContextId\"}" \
      'Target.attachToTarget {"targetId":"$targetId","flatten":true}' 'sleep 1500' \
      'S:Runtime.evaluate {"expression":"document.title","returnByValue":true}' \
      "S:Page.navigate {\"url\":\"http://127.0.0.2:$HTTP_PORT/spa.html\"}" 'sleep 2500' \
      'S:Runtime.evaluate {"expression":"document.title","returnByValue":true}' \
      "S:Page.navigate {\"url\":\"http://127.0.0.3:$HTTP_PORT/canvas.html\"}" 'sleep 2500' \
      'S:Runtime.evaluate {"expression":"document.title","returnByValue":true}' 2>&1)
  echo "$out" | grep -q '"value":"Bench Text Article"' || fails+="context "
  echo "$out" | grep -q '"value":"Bench SPA Dashboard"' && echo "$out" | grep -q '"value":"Bench Canvas Gallery"' || fails+="navigate "
  out=$("$BIN/slim-cdp" -t 20 "$B" 'Target.createBrowserContext {"proxyServer":"http://127.0.0.1:9","proxyBypassList":"127.0.0.1"}' \
      "Target.createTarget {\"url\":\"http://127.0.0.1:$HTTP_PORT/list.html\",\"browserContextId\":\"\$browserContextId\"}" \
      'Target.attachToTarget {"targetId":"$targetId","flatten":true}' 'sleep 2500' \
      'S:Runtime.evaluate {"expression":"document.title","returnByValue":true}' 2>&1)
  echo "$out" | grep -q '"value":"Bench Long List"' || fails+="proxy-context "
  # 6. input events: insertText into a field, mouse click on a fixed button
  local fws; fws=$(curl -s "http://127.0.0.1:$PORT/json/list" | jq -r '.[] | select(.title=="Bench Checkout Form") | .webSocketDebuggerUrl' | head -1)
  out=$("$BIN/slim-cdp" -t 15 "$fws" \
      'Runtime.evaluate {"expression":"var b=document.createElement(\"button\");b.style.cssText=\"position:fixed;left:0;top:0;width:80px;height:80px;z-index:9\";b.onclick=function(){window.clicked=1};document.body.appendChild(b);document.querySelector(\"[name=f1]\").focus();1"}' \
      'Input.insertText {"text":"zz"}' \
      'Input.dispatchMouseEvent {"type":"mousePressed","x":30,"y":30,"button":"left","clickCount":1}' \
      'Input.dispatchMouseEvent {"type":"mouseReleased","x":30,"y":30,"button":"left","clickCount":1}' \
      'Runtime.evaluate {"expression":"document.querySelector(\"[name=f1]\").value+\"/\"+window.clicked","returnByValue":true}' 2>&1)
  echo "$out" | grep -q '"value":"zz[a-z]*/1"' || fails+="input "
  # 7. download
  mkdir -p "$UDD/dl"
  "$BIN/slim-cdp" -t 15 "$B" "Browser.setDownloadBehavior {\"behavior\":\"allow\",\"downloadPath\":\"$UDD/dl\",\"eventsEnabled\":true}" \
      "Target.createTarget {\"url\":\"http://127.0.0.1:$HTTP_PORT/dl/report.txt\"}" 'wait Browser.downloadWillBegin' 'sleep 1500' >/dev/null 2>&1
  [ -s "$UDD/dl/report.txt" ] || fails+="download "
  # 8. 50 leased tabs at once, each answering
  local -a steps=(); for i in $(seq 50); do steps+=("Target.createTarget {\"url\":\"http://127.0.0.$(( i % 6 + 1 )):$HTTP_PORT/inner.html?$i\"}"); done
  "$BIN/slim-cdp" -t 60 "$B" "${steps[@]}" >/dev/null 2>&1 || fails+="createTarget50 "
  sleep 4
  local npages; npages=$(page_ws | wc -l)
  [ "$npages" -ge 56 ] || fails+="tabs50($npages) "
  # Regression check: with many tabs open, the OLDER (background, hidden) tabs must
  # still answer Runtime.evaluate (--enable-low-end-device-mode froze them).
  local w8 bad=0
  for w8 in $(page_ws | tail -8); do
    if [ "$(eval_ws "$w8" '1+1')" != 2 ]; then
      bad=$((bad+1))
      echo "background eval failed on $w8: $("$BIN/slim-cdp" -t 10 "$w8" 'Runtime.evaluate {"expression":"location.href","returnByValue":true}' 2>&1 | head -c 200)" >&2
    fi
  done
  [ $bad = 0 ] || fails+="eval50-background($bad/8) "
  local m50; m50=$("$BIN/slim-memstat" --json --crashpad $BPID)
  # 9. still alive
  curl -s --max-time 2 "http://127.0.0.1:$PORT/json/version" >/dev/null || fails+="browser-died "
  kill -0 $BPID 2>/dev/null || fails+="browser-exited "
  stop
  local res=${fails:-pass}
  jq -cn --arg c "$(basename "$cfg")" --arg r "$res" --arg e "$CFG_ENGINE" --argjson shot "${SHOT_LEN:-0}" --argjson m50 "$m50" \
    '{config:$c, engine:$e, date:(now|todate), smoke:($r|rtrimstr(" ")), shot_b64:$shot, tabs56:$m50}' | tee -a "$RES/smoke.jsonl"
  [ "$res" = pass ]
}

[ -n "${BENCH_LIB:-}" ] && return 0   # sourced by governor.sh / fidelity.sh

case "${1:-}" in
  run) shift; [ $# -ge 2 ] || die "usage: bench.sh run CONFIG A|B [REPS]"; cmd_run "$@" ;;
  ab) shift; [ $# -ge 3 ] || die "usage: bench.sh ab CFG_A CFG_B A|B [PAIRS]"
      for p in $(seq "${4:-3}"); do REP0=$((p - 1)) cmd_run "$1" "$3" 1; REP0=$((p - 1)) cmd_run "$2" "$3" 1; done ;;
  smoke) shift; [ $# -ge 1 ] || die "usage: bench.sh smoke CONFIG"; cmd_smoke "$1" ;;
  args) shift; config_args "$1" /tmp/UDD; echo "engine: $CFG_ENGINE" ;;
  *) sed -n '2,14p' "$0"; exit 2 ;;
esac
