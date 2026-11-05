#!/usr/bin/env bash
# Functional test of every slimium command against file:// / data: fixtures.
# Env: SLIM_BIN (binary under test, default ./slimium), SLIM_CTL (binary for start/stop, default ./slimium),
#      SLIM_EXEC (command prefix, e.g. a sanitizer sandbox), SLIM_TEST_PORT (default 9310, range 9300-9329),
#      SLIM_STATE_DIR (default: fresh temp dir), SLIM_METRICS=0 to skip /usr/bin/time measurements.
set -u
cd "$(dirname "$0")/.."
BIN=${SLIM_BIN:-./slimium}
CTL=${SLIM_CTL:-./slimium}
EXEC=${SLIM_EXEC:-}
PORT=${SLIM_TEST_PORT:-9310}
FX="$PWD/tests/fixtures"
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
own_state=0
if [ -z "${SLIM_STATE_DIR:-}" ]; then SLIM_STATE_DIR=$(mktemp -d /tmp/slim-test-state.XXXXXX); own_state=1; fi
export SLIM_STATE_DIR
UDD=$(mktemp -d /tmp/slim-test-udd.XXXXXX)
OUT=$(mktemp -d /tmp/slim-test-out.XXXXXX)
SES=t$$
METRICS=${SLIM_METRICS:-1}
[ -n "$EXEC" ] && METRICS=0
[ -x /usr/bin/time ] || METRICS=0

HPID=
cleanup() {
    [ -n "$HPID" ] && kill "$HPID" 2>/dev/null
    $CTL --session "$SES0" stop >/dev/null 2>&1
    $CTL --session "$SES0-att" stop >/dev/null 2>&1
    rm -rf "$UDD" "$OUT"
    [ "$own_state" = 1 ] && rm -rf "$SLIM_STATE_DIR"
}
SES0=$SES
trap cleanup EXIT
trap 'exit 130' INT TERM

pass=0; fail=0
: >"$OUT/metrics.tsv"
LAST=""
# run NAME EXPECTED_RC [args...] -> stdout in $LAST, stderr in $OUT/err
run() {
    local name=$1 want=$2; shift 2
    local t0 t1 rc
    t0=$(date +%s%N)
    if [ "$METRICS" = 1 ]; then
        /usr/bin/time -f '%M' -o "$OUT/time" $BIN --session "$SES" "$@" >"$OUT/out" 2>"$OUT/err" <"${STDIN:-/dev/null}"
    else
        $EXEC $BIN --session "$SES" "$@" >"$OUT/out" 2>"$OUT/err" <"${STDIN:-/dev/null}"
    fi
    rc=$?
    t1=$(date +%s%N)
    LAST=$(cat "$OUT/out")
    local rss=-; [ "$METRICS" = 1 ] && rss=$(tail -1 "$OUT/time")
    printf '%s\t%d\t%s\n' "$name" $(((t1 - t0) / 1000000)) "$rss" >>"$OUT/metrics.tsv"
    if [ "$rc" != "$want" ]; then
        echo "FAIL [$name] rc=$rc want=$want: $(head -c 300 "$OUT/err") $(head -c 200 "$OUT/out")"
        fail=$((fail + 1)); return 1
    fi
    if grep -q "ERROR: AddressSanitizer\|runtime error:\|LeakSanitizer" "$OUT/err"; then
        echo "FAIL [$name] sanitizer: $(grep -m3 'ERROR\|runtime error\|SUMMARY' "$OUT/err")"
        fail=$((fail + 1)); return 1
    fi
    return 0
}
# check NAME REGEX (grep -E on $LAST)
check() {
    if printf '%s' "$LAST" | grep -Eq -- "$2"; then pass=$((pass + 1)); else
        echo "FAIL [$1] output did not match /$2/: $(printf '%s' "$LAST" | head -c 300)"; fail=$((fail + 1)); fi
}
# t NAME RC REGEX args...
t() { local n=$1 rc=$2 p=$3; shift 3; if run "$n" "$rc" "$@"; then if [ -n "$p" ]; then check "$n" "$p"; else pass=$((pass + 1)); fi; fi; }
errcheck() { if grep -Eq -- "$2" "$OUT/err"; then pass=$((pass + 1)); else echo "FAIL [$1] stderr did not match /$2/: $(head -c 300 "$OUT/err")"; fail=$((fail + 1)); fi; }
pngh() { od -An -tu1 -j20 -N4 "$1" | awk '{print $1*16777216+$2*65536+$3*256+$4}'; }
ok() { if eval "$2"; then pass=$((pass + 1)); else echo "FAIL [$1] $3"; fail=$((fail + 1)); fi; }

HPORT=$((PORT + 18)) # local HTTP fixture server (9328 by default)
if command -v python3 >/dev/null; then
    (cd "$FX" && exec python3 -m http.server --bind 127.0.0.1 "$HPORT" >/dev/null 2>&1) &
    HPID=$!
fi
echo "== slimium functional tests (port $PORT, session $SES)"
# ---- usage / errors without a browser ----
t version 0 '^slimium [0-9]' --version
t help 0 'commands:' help
t unknown-cmd 2 '' frobnicate
t bad-port 2 '' --port 99999 tabs
t no-session 1 '' url
errcheck no-session "no session"
t no-session-json 1 '"ok":false' --json url

# ---- session lifecycle ----
t start 0 "started session $SES" start --port "$PORT" --user-data-dir "$UDD" --profile balanced --window 1000x700 || { echo "cannot start browser"; exit 1; }
t start-again 1 '' start --port "$PORT"
t status 0 'alive +yes' status
check status-mem 'pss_kb +[0-9]+'
t status-json 0 '"alive":true' --json status
t sessions 0 "$SES" sessions

# ---- navigation ----
t nav 0 'Form fixture' nav "$FX/form.html"
t url 0 'form\.html$' url
t title 0 '^Form fixture$' title
t nav-dom 0 'Links fixture' nav "$FX/links.html" --wait dom
t nav-json 0 '"ok":true,"url":"file://.*links\.html"' --json nav "$FX/links.html"
t nav-fail 1 '' nav "file:///nonexistent/slimium.html"
errcheck nav-fail 'ERR_FILE_NOT_FOUND'
t nav-data 0 'Data' nav 'data:text/html,<title>Data</title><p>x</p>'
t nav-links 0 'Links' nav "$FX/links.html"
t click-link 0 'clicked' click '#to-form'
t wait-load 0 'loaded' wait-load
t url-after-click 0 'form\.html' url
t back 0 'links\.html' back
t forward 0 'form\.html' forward
t reload 0 'Form fixture' reload
t reload-hard 0 'Form fixture' reload --hard --wait dom

# ---- eval / content ----
t eval-num 0 '^3$' eval '1+2'
t eval-str 0 '^hi there$' eval '"hi " + "there"'
t eval-obj-json 0 '"value":\{"a":1,"b":\[2,3\]\},"type":"object"' --json eval '({a:1,b:[2,3]})'
t eval-promise 0 '^42$' eval 'new Promise(r => setTimeout(() => r(42), 50))'
t eval-undef 0 '"type":"undefined"' --json eval 'undefined'
t eval-nan 0 '^NaN$' eval 'NaN'
t eval-throw 1 '' eval 'throw new Error("boom")'
errcheck eval-throw 'boom'
printf 'document.title.toUpperCase()' >"$OUT/js"
STDIN="$OUT/js" t eval-stdin 0 '^FORM FIXTURE$' eval -
t text 0 'Form' text
t text-sel 0 '^Form$' text h1
t html-sel 0 '^<h1 id="h">Form</h1>$' html h1
t html-doc 0 '^<!DOCTYPE html>' html
t html-missing 1 '' html '#missing'
errcheck html-missing 'no element matches'
t bad-selector 1 '' click '##bad'
errcheck bad-selector "bad selector '##bad'"
t xpath 0 '^Form$' text '//h1'

# ---- input ----
t click 0 'clicked #clicks' click '#clicks'
t click-verify 0 '^clicks 1$' text '#clicks'
t dblclick 0 'double-clicked' dblclick '#clicks'
t dblclick-verify 0 '^yes$' eval 'document.querySelector("#clicks").dataset.dbl'
t hover 0 'hovered' hover '#hov'
t hover-verify 0 '^hovered$' text '#hov'
t click-hidden 1 '' click '#hidden'
errcheck click-hidden 'not visible'
t type 0 'typed 5 chars' type '#name' 'ab cé'
t type-verify 0 '^ab cé\|a,b, ,c,é$' eval 'document.querySelector("#name").value + "|" + keys.join(",")'
t type-append 0 '' type '#name' 'Z'
t type-append-verify 0 '^ab céZ$' eval 'document.querySelector("#name").value'
t fill 0 'filled' fill '#name' 'Jane Doe'
t fill-verify 0 '^Jane Doe$' eval 'document.querySelector("#name").value'
t fill-textarea 0 '' fill '#bio' 'line1'
t fill-ce 0 '' fill '#ce' 'edited'
t fill-ce-verify 0 '^edited$' text '#ce'
t press-tab 0 'pressed Tab' press Tab
t press-ctrl 0 '' press Control+a
t press-ctrl-verify 0 '^a\+ctrl$' eval 'lastKey'
t press-char 0 '' press x
t press-bad 2 '' press NoSuchKey
t focus-name 0 '' click '#name'
t press-enter 0 '' press Enter
t submit-verify 0 '^submitted:Jane Doe$' text '#out'
t select-value 0 'selected b' select '#color' b
t select-label 0 'selected g' select '#color' Green
t select-verify 0 '^g$' eval 'colorChanged'
t select-bad 1 '' select '#color' Purple
t check 0 'checked' check '#agree'
t check-verify 0 '^true$' eval 'document.querySelector("#agree").checked'
t uncheck 0 'unchecked' check '#agree' --off
t radio 0 'checked' check '#large'
t reload-form 0 '' reload
t wait-late 0 'found #late' wait '#late'
t wait-visible 0 'found' wait '#h' --visible
t wait-gone 0 'found' wait '#hidden' --gone --visible
t wait-timeout 1 '' --timeout 300 wait '#never'
errcheck wait-timeout 'timeout'
t nav-long 0 'Long' nav "$FX/long.html"
t scroll 0 'scrolled to 0,[1-9]' scroll 0 400
t scroll-to 0 'scrolled to 0,[1-9][0-9]{3}' scroll-to '#bottom'
t console 0 'log	long loaded 42' --timeout 400 console
check console-warn 'warning	a warning'
check console-error 'error	an error'
t console-json 0 '"messages":\[\{"type":"log"' --json --timeout 300 console
t wait-idle 0 'idle' wait-idle --idle 200

# ---- capture ----
t shot 0 'bytes' shot "$OUT/v.png"
LAST=$(head -c 4 "$OUT/v.png" | tail -c 3); check shot-png '^PNG$'
vh=$(pngh "$OUT/v.png")
t shot-full 0 'bytes' shot "$OUT/f.png" --full
fh=$(pngh "$OUT/f.png")
ok shot-full-height '[ "$fh" -gt 5000 ] && [ "$fh" -gt "$vh" ]' "heights viewport=$vh full=$fh"
t shot-sel 0 'bytes' shot "$OUT/s.png" --selector '#bottom'
LAST=$(pngh "$OUT/s.png"); check shot-sel-height '^(49|50|51)$'
t shot-jpeg 0 '"format":"jpeg"' --json shot "$OUT/q.jpg" --quality 40
LAST=$(od -An -tx1 -N3 "$OUT/q.jpg" | tr -d ' '); check shot-jpeg-magic '^ffd8ff'
t shot-webp 0 '' shot "$OUT/w.webp"
LAST=$(head -c 12 "$OUT/w.webp" | tail -c 4); check shot-webp-magic 'WEBP'
t shot-bad 2 '' shot "$OUT/x.png" --format gif
t pdf 0 'bytes' pdf "$OUT/p.pdf"
LAST=$(head -c 5 "$OUT/p.pdf"); check pdf-magic '^%PDF-'

# ---- frames ----
t nav-iframe 0 'Iframe' nav "$FX/iframe.html"
t frames 0 'name=inner' frames
check frames-second 'about:srcdoc'
t frames-json 0 '"depth":1' --json frames

# ---- cookies / storage ----
t cookie-set 0 'set' cookie-set sid abc123 --domain example.com --path /
t cookies 0 'sid=abc123	example.com	/' cookies
t cookies-json 0 '"cookies":\[\{"name":"sid"' --json cookies
t cookie-nodomain 1 '' cookie-set x y
t cookies-clear 0 'cleared' cookies-clear
t cookies-empty 0 '' cookies
ok cookies-empty-out '[ -z "$LAST" ]' "cookies not cleared: $LAST"
t nav-form2 0 '' nav "$FX/form.html"
t ls-set 0 '^1$' eval 'localStorage.setItem("k","v");localStorage.length'
t storage-clear 0 'storage cleared' storage-clear
t ls-verify 0 '^0$' eval 'localStorage.length'

# ---- emulation (persisted per session, re-applied on every attach) ----
t viewport 0 'viewport 600x400' viewport 600 400
t viewport-persist 0 '^\[600,400\]$' eval '[innerWidth,innerHeight]'
t viewport-mobile 0 'mobile' viewport 400 700 --scale 2 --mobile
t viewport-dpr 0 '^2$' eval 'devicePixelRatio'
t viewport-reset 0 'reset' viewport reset
t viewport-reset-dpr 0 '^1$' eval 'devicePixelRatio'
t viewport-bad 2 '' viewport 0 x
t ua 0 'set' ua 'SlimiumTest/1.0'
t ua-persist 0 '^SlimiumTest/1.0$' eval 'navigator.userAgent'
t ua-reset 0 'reset' ua --reset
t ua-default 0 'Chrome' eval 'navigator.userAgent'
t offline-on 0 'offline on' offline on
t offline-verify 0 '^false$' eval 'navigator.onLine'
t offline-off 0 'offline off' offline off
t online-verify 0 '^true$' eval 'navigator.onLine'
if [ -n "$HPID" ]; then
    # Network.setBlockedURLs blocks subresource requests (not top-level navigations)
    t http-nav 0 'Form fixture' nav "http://127.0.0.1:$HPORT/form.html"
    t http-cookie 0 'set' cookie-set hc 1
    t http-cookies 0 "hc=1	127.0.0.1" cookies
    t block 0 'blocking 1 pattern' block '*long.html'
    t block-fetch 0 '^blocked$' eval "fetch('/long.html').then(r => r.status, e => 'blocked')"
    t unblock 0 'unblocked' block
    t unblock-fetch 0 '^200$' eval "fetch('/long.html').then(r => r.status, e => 'blocked')"
    t http-idle 0 'idle' wait-idle
    t cookies-clear2 0 'cleared' cookies-clear
fi
t download-dir 0 'downloads go to' download-dir "$OUT/dl"
t download-off 0 'default' download-dir off

# ---- tabs / contexts ----
t nav-long2 0 'Long' nav "$FX/long.html"
t tabs 0 '^\* [0-9A-F]{32}	file://' tabs
T1=$(printf '%s' "$LAST" | awk '/^\*/{print $2}')
t open 0 'Links fixture' open "$FX/links.html"
T2=$(printf '%s' "$LAST" | cut -f1)
t tabs-two 0 "\* $T2" tabs
t title-new 0 '^Links fixture$' title
t switch 0 'Long fixture' switch "${T1:0:8}"
t title-switched 0 '^Long fixture$' title
t tab-opt 0 '^Links fixture$' --tab "$T2" title
t switch-bad 1 '' switch FFFFFFFFFFFF
t freeze 0 'frozen' freeze "$T2"
t thaw 0 'active' thaw "$T2"
t thaw-eval 0 '^Links fixture$' --tab "$T2" title
t freeze-cur 0 'frozen' freeze
t thaw-cur 0 'active' thaw
t close 0 "closed $T2" close "$T2"
t context-new 0 '^[0-9A-F]{32}	[0-9A-F]{32}$' context-new --url "$FX/form.html"
CTX=$(printf '%s' "$LAST" | cut -f1)
t contexts 0 "$CTX" contexts
t ctx-title 0 '^Form fixture$' title
t context-close 0 'closed context' context-close "$CTX"
t after-ctx 0 '' title
t close-cur 0 'closed' close
t auto-tab 0 '' url

# ---- memory ----
t mem 0 'browser pid [0-9]+ procs [0-9]+ pss [0-9]+ KB' mem
check mem-tab 'tab [0-9A-F]{32} heap'
t mem-json 0 '"pss_kb":[0-9]+' --json mem
t purge 0 'gc on [0-9]+ tab' purge
t after-purge 0 '^2$' eval '1+1'

# ---- raw cdp ----
t cdp-browser 0 '"product":"(Headless)?Chrome/' cdp Browser.getVersion
t cdp-page 0 '"value":7' cdp Runtime.evaluate '{"expression":"3+4"}'
t cdp-badjson 2 '' cdp Runtime.evaluate '{nope'
t cdp-error 1 '' cdp Nope.method
t cdp-target 0 'targetInfos' cdp Target.getTargets --target browser
t cdp-events 0 'Runtime.executionContextCreated' cdp Runtime.enable --events 300

# ---- batch: one connection, one JSON line per command ----
cat >"$OUT/batch" <<EOF
nav $FX/form.html
title
type #name "hello world"
eval "document.querySelector('#name').value"
# comment line

bogus
text '#h'
EOF
STDIN="$OUT/batch" run batch 1 batch
LAST=$(cat "$OUT/out"); n=$(printf '%s\n' "$LAST" | wc -l)
ok batch-lines '[ "$n" = 6 ]' "expected 6 lines, got $n: $LAST"
check batch-eval '"cmd":"eval","value":"hello world"'
check batch-err '"ok":false,"cmd":"bogus"'
check batch-text '"cmd":"text","value":"Form"'
printf 'url\ntitle\n' >"$OUT/batch2"
STDIN="$OUT/batch2" t batch-ok 0 '"cmd":"title","value":"Form fixture"' batch
printf 'stop\n' >"$OUT/batch3"
STDIN="$OUT/batch3" t batch-nostop 1 'not allowed in batch' --json batch

# ---- --port mode and attach (adopt an existing browser) ----
t port-status 0 'alive +yes' --port "$PORT" status
t port-tabs 0 'file://' --port "$PORT" tabs
t port-eval 0 '^2$' --port "$PORT" eval '1+1'
t port-none 1 '' --port 9329 status
SES="$SES0-att"
t attach 0 "attached session $SES to port $PORT" attach --port "$PORT"
t attached-title 0 '' title
t attached-stop 0 'detached' stop
SES=$SES0
t still-alive 0 'alive +yes' status

# ---- wrapper (argv[0] = slimium-chrome) ----
WRAP=$(dirname "$BIN")/slimium-chrome
LAST=$($EXEC "$WRAP" --version 2>&1); check wrapper-version '^(Chromium|Google Chrome) [0-9]+\.'
LAST=$($EXEC "$BIN" chrome --version 2>&1); check chrome-subcmd-version '^(Chromium|Google Chrome) [0-9]+\.'
if [ -z "$EXEC" ]; then
    WL="$OUT/wrapper.log"
    LAST=$(SLIM_WRAPPER_LOG="$WL" SLIM_PROFILE=max timeout 60 "$WRAP" --headless=new --user-data-dir="$OUT/wudd" --no-first-run \
        --dump-dom "data:text/html,<p id=w>wrapped</p>" 2>/dev/null)
    check wrapper-dump '<p id="w">wrapped</p>'
    LAST=$(cat "$WL"); check wrapper-log 'renderer-process-limit'
    check wrapper-log-args 'dump-dom'
    LAST=$(SLIM_CHROMIUM=/nonexistent "$WRAP" --version 2>&1; echo "rc=$?"); check wrapper-noengine 'rc=127'
fi

# ---- stop ----
t stop 0 "stopped session $SES" stop
t stop-again 1 '' stop
t status-stopped 1 '' status
ok stopped-port '! "$CTL" --port "$PORT" status >/dev/null 2>&1' "browser still answering on $PORT"

echo "== $pass passed, $fail failed"
if [ "$METRICS" = 1 ]; then
    cp "$OUT/metrics.tsv" /tmp/slimium-metrics.tsv
    awk -F'\t' '$3!="-"{n++; s+=$2; if($3>m){m=$3; mc=$1}} END{printf "== %d timed commands: avg %.1f ms, peak slimium RSS %d KB (%s)\n", n, s/n, m, mc}' "$OUT/metrics.tsv"
    echo "== per-command table (name, ms, max RSS KB): /tmp/slimium-metrics.tsv"
fi
[ "$fail" = 0 ]
