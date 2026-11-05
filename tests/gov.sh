#!/usr/bin/env bash
# tests/gov.sh: the memory governor on browsers this script starts itself (CDP 9440-9442, temp
# profiles and state dir). Checks: the governor attaches (start + wrapper), runs pressure + GC,
# reclaims only cgroups that hold nothing but the browser tree (an injected foreign process and an
# unscoped launch are skipped), opt-in hibernation restores, pages still work (eval in every tab,
# the oldest of 50+ tabs still answers, screenshot pixels exact), the wrapper keeps the engine pid,
# writes nothing to stdout and spawns no governor for --version, the governor exits with the
# engine, its own memory is small, and the user's cu is untouched.
#   tests/gov.sh            (needs a built ./slimium; chromium-browser in PATH or SLIM_CHROMIUM)
set -u
cd "$(dirname "$0")/.." || exit 2
BIN=$PWD/slimium WRAP=$PWD/slimium-chrome FX=$PWD/tests/fixtures
PA=${SLIM_GOV_TEST_PORT:-9440} PB=$((PA + 1)) PW=$((PA + 2))
NTABS=${SLIM_GOV_TEST_TABS:-50}
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
source tests/cu/lib.sh
ST=$(mktemp -d /tmp/slim-gov-test.XXXXXX)
export SLIM_STATE_DIR=$ST SLIM_GOV_LOG=$ST/gov.log SLIM_GOV_IDLE_MS=2000 SLIM_GOV_RECLAIM_S=8
unset SLIM_GOV SLIM_SCOPE SLIM_GOV_HIBERNATE
WPID= SPID= BG=
cleanup() {
    "$BIN" --session ga stop >/dev/null 2>&1
    "$BIN" --session gb stop >/dev/null 2>&1
    "$BIN" --session gc stop >/dev/null 2>&1
    [ -n "$WPID" ] && kill "$WPID" 2>/dev/null
    [ -n "$SPID" ] && kill "$SPID" 2>/dev/null
    [ -n "$BG" ] && kill "$BG" 2>/dev/null
    sleep 0.5
    rm -rf "${ST:?}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
pass=0 fail=0
ok() { if eval "$2"; then pass=$((pass + 1)); else echo "FAIL [$1] ${3:-}"; fail=$((fail + 1)); fi; }
jget() { python3 -c 'import sys,json; d=json.loads(sys.stdin.readline()); v=d.get(sys.argv[1], ""); print(v if not isinstance(v,bool) else str(v).lower())' "$1"; }
wait_log() { # wait_log FILE REGEX SECONDS
    local i; for i in $(seq 1 $(($3 * 10))); do grep -Eq -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1
}
gone() { local i; for i in $(seq 1 $(($2 * 10))); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; return 1; }
child_cg() { local c; c=$(cat /proc/"$1"/task/*/children 2>/dev/null | tr ' ' '\n' | grep -m1 .); cut -d: -f3 /proc/"$c"/cgroup 2>/dev/null; }
MYCG=$(cut -d: -f3 /proc/self/cgroup)
before=$(user_cu_fingerprint)
echo "== slimium governor tests (ports $PA-$PW, state $ST)"

# ---- session with governor (default on) ----
out=$("$BIN" --session ga --json start --port "$PA" --user-data-dir "$ST/udd-a")
ok start '[ "$(echo "$out" | jget ok)" = true ]' "$out"
P=$(echo "$out" | jget pid) G=$(echo "$out" | jget governor)
ok governor-spawned '[ "${G:-0}" -gt 0 ] && kill -0 "$G"' "governor=$G"
ok governor-not-child '[ "$(awk "{print \$4}" /proc/$G/stat)" != "$P" ]' "governor is a child of the browser"
ok governor-cmdline '! tr "\0" " " </proc/$G/cmdline | grep -q remote-debugging-port' "governor cmdline would match find_browser_pid"
ok status-governor '"$BIN" --session ga --json status | grep -q "\"governor\":$G"'
ok status-iso '"$BIN" --session ga --json status | grep -Eq "\"iso_kb\":[0-9]+.*\"file_union_kb\":[0-9]+"'
ok mem-iso '"$BIN" --session ga mem | grep -Eq "iso [0-9]+ KB = anon [0-9]+ \+ shmem [0-9]+ \+ file_union [0-9]+ KB \| governor pid $G"'
# a second browser (the wrapper, as cu runs it) at the same time: each gets its own engine copy
n0=$(pgrep -fc '^slimium gov ')
SLIM_GOV_LOG=$ST/w.log "$WRAP" --headless=new --remote-debugging-port="$PW" --user-data-dir="$ST/udd-w" --no-first-run about:blank >"$ST/w.out" 2>/dev/null &
WPID=$!
for i in $(seq 1 100); do "$BIN" --port "$PW" status >/dev/null 2>&1 && break; sleep 0.1; done
EA=$(readlink /proc/"$P"/exe) EW=$(readlink /proc/"$WPID"/exe)
ok engine-copy "case \$EA in */slimium/engines/*) true;; *) false;; esac" "session engine $EA"
ok engine-copy-per-browser '[ "$EA" != "$EW" ] && case $EW in */slimium/engines/*) true;; *) false;; esac' "$EA vs $EW"
SCG=$(child_cg "$P")
ok scoped "case \$SCG in */slimium-$P-*.scope) true;; *) false;; esac" "zygote cgroup $SCG"
for f in form links long iframe render; do "$BIN" --session ga open "file://$FX/$f.html" >/dev/null; done
ok gc-pass 'wait_log "$SLIM_GOV_LOG" "\"ev\":\"pass\".*\"tabs_gc\":[1-9]" 40' "no pressure+GC pass in the log"
ok reclaim 'wait_log "$SLIM_GOV_LOG" "\"ev\":\"reclaim\".*slimium-$P-" 40' "no reclaim of the browser scope"
# every reclaimed cgroup is one of the browser's own scopes, never ours / the user's
bad=$(grep '"ev":"reclaim"' "$SLIM_GOV_LOG" | grep -o '"cgroup":"[^"]*"' | sort -u | grep -Ev "slimium-$P-|app-org\.chromium\.Chromium-$P\.scope")
ok reclaim-only-own '[ -z "$bad" ]' "reclaimed: $bad"
ok reclaim-not-mine '! grep "\"ev\":\"reclaim\"" "$SLIM_GOV_LOG" | grep -q "\"cgroup\":\"$MYCG\""'
ok reclaim-not-cu '! grep "\"ev\":\"reclaim\"" "$SLIM_GOV_LOG" | grep -q "cu-daemon"'
# ---- an injected foreign process makes the scope off limits ----
sleep 300 & SPID=$!
if echo "$SPID" >"/sys/fs/cgroup$SCG/cgroup.procs" 2>/dev/null; then
    o=$("$BIN" --session ga --json gov --once)
    ok foreign-skipped '[ "$(echo "$o" | jget cgroups_foreign)" -ge 1 ]' "$o"
    ok foreign-logged 'grep -q "\"ev\":\"skip-foreign\",.*\"cgroup\":\"$SCG\"" "$SLIM_GOV_LOG"'
    n=$(grep -c "\"ev\":\"reclaim\".*\"cgroup\":\"$SCG\"" "$SLIM_GOV_LOG")
    kill "$SPID"; wait "$SPID" 2>/dev/null; SPID=
    o=$("$BIN" --session ga --json gov --once)
    ok foreign-gone '[ "$(echo "$o" | jget cgroups_foreign)" = 0 ] && [ "$(echo "$o" | jget cgroups_reclaimed)" -ge 1 ]' "$o"
    ok reclaimed-after-removal '[ "$(grep -c "\"ev\":\"reclaim\".*\"cgroup\":\"$SCG\"" "$SLIM_GOV_LOG")" -gt "$n" ]'
else
    echo "note: cannot move a test process into $SCG (injected-foreign check skipped)"
    kill "$SPID"; SPID=
fi
o=$("$BIN" --session ga gov --once)
ok once-text 'echo "$o" | grep -Eq "^iso [0-9.]+ -> [0-9.]+ MB"' "$o"
# ---- opt-in hibernation: about:blank, restored when a client attaches, all restored on exit ----
SLIM_GOV_LOG=$ST/hib.log SLIM_GOV_HIBERNATE_MIN_S=0 "$BIN" --session ga gov --hibernate 1 --idle-ms 1000 --reclaim-s 0 >/dev/null 2>&1 & BG=$!
ok hibernated 'wait_log "$ST/hib.log" "\"ev\":\"hibernate\"" 30' "no tab hibernated"
T=$(grep -m1 '"ev":"hibernate"' "$ST/hib.log" | grep -o '"tab":"[0-9A-F]*"' | cut -d'"' -f4)
U=$(grep -m1 '"ev":"hibernate"' "$ST/hib.log" | grep -o '"url":"[^"]*"' | cut -d'"' -f4)
[ -n "$T" ] && "$BIN" --session ga --tab "$T" url >/dev/null
ok hib-restore-on-attach 'wait_log "$ST/hib.log" "\"ev\":\"restore\".*\"tab\":\"$T\".*\"ok\":1" 10'
sleep 1
ok hib-restored-url '[ "$("$BIN" --session ga --tab "$T" url)" = "$U" ]' "url $("$BIN" --session ga --tab "$T" url) want $U"
kill -TERM "$BG"; gone "$BG" 15; BG=
nh=$(grep -c '"ev":"hibernate"' "$ST/hib.log") nr=$(grep -c '"ev":"restore".*"ok":1' "$ST/hib.log")
ok hib-all-restored '[ "$nh" = "$nr" ]' "hibernated $nh restored $nr"
# ---- pages still work after reclaim: every tab evaluates, screenshot pixels exact ----
"$BIN" --session ga gov --once >/dev/null
for t in $("$BIN" --session ga tabs | awk '{print $1}' | tr -d '*'); do
    ok "eval-${t:0:8}" '[ "$("$BIN" --session ga --tab "$t" --timeout 10000 eval "1+1")" = 2 ]'
done
"$BIN" --session ga nav "file://$FX/render.html" >/dev/null
"$BIN" --session ga gov --once >/dev/null
"$BIN" --session ga shot "$ST/r.png" >/dev/null
{ printf "new Promise(function(res){var i=new Image();i.onload=function(){var c=document.createElement('canvas');"
  printf "c.width=i.width;c.height=i.height;var x=c.getContext('2d');x.drawImage(i,0,0);"
  printf "function p(X,Y){return Array.prototype.slice.call(x.getImageData(X,Y,1,1).data,0,3).join(',')}"
  printf "res([p(50,50),p(250,50),p(450,50),p(50,250),p(250,250),p(450,250),p(405,205),p(602,50)].join(' '))};"
  printf "i.onerror=function(){res('decode failed')};i.src='data:image/png;base64,%s'})" "$(base64 -w0 "$ST/r.png")"; } >"$ST/px.js"
px=$("$BIN" --session ga eval - <"$ST/px.js")
ok render-pixels 'echo "$px" | grep -Eq "^255,0,0 0,255,0 0,0,255 255,255,0 255,0,255 0,255,255 255,255,255 [0-9],[0-9],[0-9]$"' "$px"
# ---- many tabs: the OLDEST of $NTABS still answers while the governor runs passes ----
OLD=$("$BIN" --session ga --json open "data:text/html,<title>t0</title><script>var born=Date.now()</script>" --background | jget id)
for i in $(seq 1 $((NTABS - 1))); do "$BIN" --session ga open "data:text/html,<title>t$i</title><p>$i" --background >/dev/null; done
m=$(grep -c '"ev":"pass".*"tabs_gc"' "$SLIM_GOV_LOG")
"$BIN" --session ga --tab "$OLD" eval 'born' >/dev/null   # activity: re-arms the GC pass
ok many-tabs-pass 'for i in $(seq 1 300); do [ "$(grep -c "\"ev\":\"pass\".*\"tabs_gc\"" "$SLIM_GOV_LOG")" -gt "$m" ] && break; sleep 0.1; done; grep "\"ev\":\"pass\"" "$SLIM_GOV_LOG" | tail -1 | grep -Eq "\"tabs_gc\":([5-9][0-9]|[1-9][0-9][0-9])"' "no GC pass over $NTABS+ tabs: $(grep '"ev":"pass"' "$SLIM_GOV_LOG" | tail -1)"
"$BIN" --session ga gov --once >/dev/null
sleep 3
ok oldest-tab-answers '[ "$("$BIN" --session ga --tab "$OLD" --timeout 5000 eval "document.title+(born>0)")" = t0true ]' "oldest of $NTABS tabs did not answer"
ok oldest-tab-raf '[ "$("$BIN" --session ga --tab "$OLD" --timeout 5000 eval "new Promise(r=>setTimeout(()=>r(42),10))")" = 42 ]' "timers of the oldest tab do not run"
# ---- the governor's own footprint ----
pss=$(awk '/^Pss:/{print $2}' /proc/"$G"/smaps_rollup) anon=$(awk '/^RssAnon:/{print $2}' /proc/"$G"/status)
rss=$(awk '/^VmRSS:/{print $2}' /proc/"$G"/status)
ok governor-small '[ "$pss" -lt 2048 ] && [ "$anon" -lt 1024 ]' "pss ${pss} KB anon ${anon} KB"
echo "   governor: rss ${rss} KB (pss ${pss}, anon ${anon}), cpu $(ps -o time= -p "$G") over $(ps -o etime= -p "$G")"
# ---- governor exits with the engine ----
"$BIN" --session ga stop >/dev/null
ok governor-exits 'gone "$G" 10' "governor $G still alive after the browser stopped"
ok exit-logged 'grep -q "\"ev\":\"exit\",\"pid\":$P" "$SLIM_GOV_LOG"'

# ---- system library copies (list recorded by the governor above): a new launch maps them ----
out=$("$BIN" --session gc --json start --port "$PB" --user-data-dir "$ST/udd-c")
PC=$(echo "$out" | jget pid)
ok lib-copies 'grep -q "/slimium/engines/[^ ]*/lib/lib" /proc/$PC/maps' "no private library copy mapped"
ok lib-no-glibc '! grep -Eq "/slimium/engines/[^ ]*/lib/(libc\.so|ld-linux|libm\.so)" /proc/$PC/maps'
ok lib-eval '[ "$("$BIN" --session gc eval "1+1")" = 2 ]'
"$BIN" --session gc stop >/dev/null

# ---- unscoped launch (SLIM_SCOPE=0, --no-gov): the shared cgroup is never reclaimed ----
out=$(SLIM_SCOPE=0 "$BIN" --session gb --json start --port "$PB" --user-data-dir "$ST/udd-b" --no-gov)
PBP=$(echo "$out" | jget pid)
ok no-gov '[ "$(echo "$out" | jget governor)" = 0 ]' "$out"
UCG=$(child_cg "$PBP")
ok unscoped "case \$UCG in */slimium-*) false;; *) true;; esac" "$UCG"
o=$("$BIN" --session gb --json gov --once)
ok unscoped-foreign '[ "$(echo "$o" | jget cgroups_foreign)" -ge 1 ]' "$o"
ok unscoped-not-reclaimed '! grep "\"ev\":\"reclaim\"" "$SLIM_GOV_LOG" | grep -q "\"cgroup\":\"$UCG\""'
"$BIN" --session gb stop >/dev/null

# ---- wrapper: pid kept, governor detached, nothing on stdout, none for --version ----
n0=$(pgrep -fc '^slimium gov ')
v=$("$WRAP" --version 2>&1)
ok wrapper-version 'echo "$v" | grep -Eq "^(Chromium|Google Chrome) [0-9]+\."' "$v"
sleep 0.3
ok version-no-governor '[ "$(pgrep -fc "^slimium gov ")" = "$n0" ]'
ok wrapper-pid '[ "$("$BIN" --port "$PW" --json status | jget pid)" = "$WPID" ]' "engine pid is not the pid we spawned"
WG=$("$BIN" --port "$PW" --json status | jget governor)
ok wrapper-governor '[ "${WG:-0}" -gt 0 ] && [ "$(awk "{print \$4}" /proc/$WG/stat)" != "$WPID" ]' "governor=$WG"
ok wrapper-scoped "child_cg $WPID | grep -q /slimium-$WPID-"
ok wrapper-reclaim 'wait_log "$ST/w.log" "\"ev\":\"reclaim\".*slimium-$WPID-" 40'
ok wrapper-stdout '[ ! -s "$ST/w.out" ]' "engine stdout: $(head -c 200 "$ST/w.out")"
kill -TERM "$WPID"; wait "$WPID" 2>/dev/null; WPID=
ok wrapper-governor-exits 'gone "$WG" 10'

after=$(user_cu_fingerprint)
ok user-cu-untouched '[ "$before" = "$after" ]' "$before | $after"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
