#!/usr/bin/env bash
# tests/emulation.sh: persistent emulation (keeper), touch/orientation/presets/UA, dialog policy.
# Uses only CDP 9572-9579 and fixture HTTP 9588 by default (SLIM_EMU_PORT, SLIM_EMU_HTTP_PORT),
# fresh /tmp state, profiles and browser data; never touches cu or named user profiles.
set -u
cd "$(dirname "$0")/.."
BIN=$(printenv SLIM_BIN || true); [ -n "$BIN" ] || BIN=./slimium
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
P0=${SLIM_EMU_PORT:-9572}
HP=${SLIM_EMU_HTTP_PORT:-9588}
case "$P0" in 957[2-6]) ;; *) echo "emulation tests need SLIM_EMU_PORT 9572-9576 (uses 4 consecutive ports)"; exit 2;; esac
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
for p in $P0 $((P0+1)) $((P0+2)) $((P0+3)) $HP; do
    if (echo >/dev/tcp/127.0.0.1/$p) 2>/dev/null; then echo "port $p is in use; refusing to run"; exit 1; fi
done
OUT=$(mktemp -d /tmp/slim-emu-test.XXXXXX)
export SLIM_STATE_DIR="$OUT/state" SLIM_PROFILES_ROOT="$OUT/profiles"
unset SLIM_USER_PROFILE SLIM_HOLD SLIM_KEEP_HEADLESS_UA SLIM_SESSION
mkdir -p "$SLIM_STATE_DIR" "$SLIM_PROFILES_ROOT"
SES=emu$$; SGOV=emugov$$; SOLD=emuold$$; SKILL=emukill$$; SATT=emuatt$$
pass=0; fail=0; LAST=; LASTMS=0; MAXMS=0; MAXNAME=
cleanup() {
    for s in "$SES" "$SGOV" "$SOLD" "$SKILL" "$SATT"; do "$BIN" --session "$s" stop >/dev/null 2>&1; done
    [ -n "${HTTP_PID:-}" ] && kill "$HTTP_PID" 2>/dev/null
    if [ "${SLIM_EMU_KEEP:-}" = 1 ]; then echo "artifacts: $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# fixture server: /story/* serves the story editor (client-side routes survive a reload)
python3 - "$HP" "$PWD/tests/fixtures" >"$OUT/http.log" 2>&1 <<'PY' &
import http.server, os, sys
root = sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def translate_path(self, path):
        if path.split('?')[0].startswith('/story'): return os.path.join(root, 'emu-story.html')
        return super().translate_path(path)
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
PY
HTTP_PID=$!
for _ in $(seq 50); do (echo >/dev/tcp/127.0.0.1/$HP) 2>/dev/null && break; sleep 0.1; done
URL="http://127.0.0.1:$HP/emu-loads.html"

# runs SESSION NAME WANT_RC ARGS...: timed, output in $LAST, duration in $LASTMS
runs() {
    local s=$1 name=$2 want=$3; shift 3
    local t0 t1 rc ms
    t0=$(date +%s%N)
    "$BIN" --session "$s" "$@" >"$OUT/stdout" 2>"$OUT/stderr" </dev/null; rc=$?
    t1=$(date +%s%N); ms=$(((t1-t0)/1000000)); LAST=$(cat "$OUT/stdout"; cat "$OUT/stderr")
    LASTMS=$ms
    printf '%s\t%d\n' "$name" "$ms" >>"$OUT/metrics.tsv"
    case " $* " in *" start "*|*" stop "*) ;; *) if [ "$ms" -gt "$MAXMS" ]; then MAXMS=$ms; MAXNAME=$name; fi;; esac
    if [ "$rc" != "$want" ]; then echo "FAIL [$name] rc=$rc want=$want: $(printf '%s' "$LAST" | head -c 400)"; fail=$((fail+1)); return 1; fi
    return 0
}
check() { if printf '%s' "$LAST" | grep -Eq -- "$2"; then pass=$((pass+1)); else echo "FAIL [$1] wanted /$2/: $(printf '%s' "$LAST" | head -c 400)"; fail=$((fail+1)); fi; }
ts() { local s=$1 n=$2 rc=$3 p=$4; shift 4; if runs "$s" "$n" "$rc" "$@"; then if [ -n "$p" ]; then check "$n" "$p"; else pass=$((pass+1)); fi; fi; }
t() { local n=$1 rc=$2 p=$3; shift 3; ts "$SES" "$n" "$rc" "$p" "$@"; }
ok() { if eval "$2"; then pass=$((pass+1)); else echo "FAIL [$1] $3"; fail=$((fail+1)); fi; }

keepers() { pgrep -fc -- "__hold --session $1\$" || true; }
keeper_pid() { pgrep -f -- "__hold --session $1\$" | head -1; }
nloads() { "$BIN" --session "${1:-$SES}" eval 'JSON.parse(localStorage.getItem("loads")||"[]").length' 2>/dev/null || echo -1; }
# wait until the page recorded at least N loads (it reloads itself after the command returned)
wait_loads() { local i n; for i in $(seq 60); do n=$(nloads "${2:-$SES}"); [ "$n" -ge "$1" ] 2>/dev/null && return 0; sleep 0.1; done; return 1; }
# chk_load NAME URLPART W H ORIENT DPR COARSE [SESSION]: the last load of that URL saw exactly this
chk_load() {
    local v
    v=$("$BIN" --session "${8:-$SES}" eval "JSON.stringify(JSON.parse(localStorage.getItem('loads')||'[]').filter(function(l){return l.url.indexOf('$2')>=0}).slice(-1)[0]||null)" 2>&1)
    if python3 - "$v" "$3" "$4" "$5" "$6" "$7" <<'PY'
import json, sys
v = json.loads(sys.argv[1]); w, h, o, dpr, coarse = sys.argv[2:7]
ok = v is not None and v['w'] == int(w) and v['h'] == int(h) and v['o'] == o and abs(v['dpr'] - float(dpr)) < 1e-9 \
    and v['coarse'] == (coarse == 'true') and ((v['touch'] >= 1) == (coarse == 'true'))
sys.exit(0 if ok else 1)
PY
    then pass=$((pass+1)); else echo "FAIL [$1] load of $2 saw $v, wanted ${3}x$4 $5 dpr $6 coarse $7"; fail=$((fail+1)); fi
}
lat() { local s=$1 t0 t1 i; t0=$(date +%s%N); for i in 1 2 3 4 5 6 7 8 9 10; do "$BIN" --session "$s" eval 1 >/dev/null 2>&1; done; t1=$(date +%s%N); awk -v n=$(((t1-t0)/10000)) 'BEGIN{printf "%.1f", n/100}'; }

echo "== slimium emulation tests (CDP $P0-$((P0+3)), HTTP $HP, session $SES)"
# ---------------- (e) lifecycle: start spawns one keeper ----------------
t start 0 'keeper pid [0-9]+' start --port "$P0" --user-data-dir "$OUT/udd-main" --no-gov --window 1000x700 || { echo "cannot start browser"; exit 1; }
ok keeper-one '[ "$(keepers "$SES")" = 1 ]' "want exactly one keeper, got $(keepers "$SES")"
KP=$(keeper_pid "$SES")
t status-keeper 0 "keeper +$KP" status
t nav 0 'Emulation loads' nav "$URL"
t clear 0 '' eval 'localStorage.clear(),1'
t default-ua-not-headless 0 '^Mozilla/5.0 \(X11; Linux [^)]*\) AppleWebKit/537.36 \(KHTML, like Gecko\) Chrome/[0-9]+\.0\.0\.0 Safari/537.36$' eval 'navigator.userAgent'
t default-uadata 0 '^Linux,false,[1-9]' eval '[navigator.userAgentData.platform,navigator.userAgentData.mobile,navigator.userAgentData.brands.length]+""'

# ---------------- (a) viewport survives every kind of load ----------------
t viewport 0 'viewport 390x844 mobile touch portrait dpr 3' viewport 390 844 --mobile --scale 3
ok viewport-fast "[ $LASTMS -lt 3000 ]" "viewport took $LASTMS ms"
t live 0 '^\[390,844,"portrait-primary",0,5,true,3,390,844\]$' eval '[innerWidth,innerHeight,screen.orientation.type,screen.orientation.angle,navigator.maxTouchPoints,matchMedia("(pointer: coarse)").matches,devicePixelRatio,screen.width,screen.height]'
n=$(nloads)
t page-self-reload 0 'tapped|clicked' click '#rl'
ok self-reload-done "wait_loads $((n+1))" "page did not reload itself"
chk_load self-reload 'emu-loads.html' 390 844 portrait-primary 3 true
t slim-reload 0 'Emulation loads' reload
chk_load slim-reload 'emu-loads.html' 390 844 portrait-primary 3 true
t client-route 0 '' click '#spa'
t client-route-live 0 '^\[390,844,"portrait-primary","\?spa=1"\]$' eval '[innerWidth,innerHeight,screen.orientation.type,location.search]'
n=$(nloads)
t page-navigates 0 '' click '#go'
ok page-nav-done "wait_loads $((n+1))" "page did not navigate itself"
chk_load page-navigation '?nav=1' 390 844 portrait-primary 3 true
t open-tab 0 'tab=1' open "$URL?tab=1"
chk_load new-tab-first-load '?tab=1' 390 844 portrait-primary 3 true
n=$(nloads)
t popup 0 '' click '#pop'
ok popup-done "wait_loads $((n+1))" "popup did not load"
chk_load popup-first-load '?popup=1' 390 844 portrait-primary 3 true
t tabs 0 'popup=1' tabs
# the popup came to the front; the agent's current tab is still tab=1: bring it back
TAB1=$("$BIN" --session "$SES" --json tabs | python3 -c 'import json,sys;print([t["id"] for t in json.load(sys.stdin)["tabs"] if t["current"]][0])')
t switch-back 0 'tab=1' switch "$TAB1"
t landscape 0 'viewport 844x390 mobile touch landscape' viewport 844 390 --mobile --scale 3
t landscape-reload 0 '' reload
chk_load landscape 'tab=1' 844 390 landscape-primary 3 true
t landscape-angle 0 '^90$' eval 'screen.orientation.angle'
t forced-portrait 0 'portrait' viewport 844 390 --mobile --scale 2 --orientation portrait
t forced-portrait-dom 0 '^\["portrait-primary",0,2\]$' eval '[screen.orientation.type,screen.orientation.angle,devicePixelRatio]'
t no-touch 0 'viewport 600x800 mobile portrait' viewport 600 800 --mobile --no-touch
t no-touch-dom 0 '^\[0,false\]$' eval '[navigator.maxTouchPoints,matchMedia("(pointer: coarse)").matches]'
t reset 0 'viewport reset' viewport reset
t reset-reload 0 '' reload
t reset-dom 0 '^\[1000,[0-9]+,"landscape-primary",0,false,1\]$' eval '[innerWidth,innerHeight,screen.orientation.type,navigator.maxTouchPoints,matchMedia("(pointer: coarse)").matches,devicePixelRatio]'

# presets: size, DPR, touch and a UA derived from the real engine version
MAJOR=$("$BIN" --session "$SES" eval 'navigator.userAgent.match(/Chrome\/(\d+)/)[1]')
t preset-mobile 0 'viewport 390x844 mobile touch portrait dpr 3' viewport mobile
t preset-mobile-reload 0 '' reload
chk_load preset-mobile 'tab=1' 390 844 portrait-primary 3 true
t preset-mobile-ua 0 "^Mozilla/5.0 \(Linux; Android 14; Pixel 8\) AppleWebKit/537.36 \(KHTML, like Gecko\) Chrome/$MAJOR.0.0.0 Mobile Safari/537.36\$" eval 'navigator.userAgent'
t preset-mobile-uadata 0 '^true,Android,Pixel 8,14.0.0$' eval 'navigator.userAgentData.getHighEntropyValues(["model","platformVersion"]).then(v=>[navigator.userAgentData.mobile,navigator.userAgentData.platform,v.model,v.platformVersion]+"")'
t explicit-ua-wins 0 'set' ua 'EmuTest/1.0'
t explicit-ua 0 '^EmuTest/1.0$' eval 'navigator.userAgent'
t explicit-ua-reload 0 '' reload
t explicit-ua-persist 0 '^EmuTest/1.0$' eval 'navigator.userAgent'
t ua-reset 0 'reset' ua --reset
t ua-preset-back 0 'Android 14; Pixel 8' eval 'navigator.userAgent'
t preset-tablet 0 'viewport 768x1024 mobile touch portrait dpr 2' viewport tablet
t preset-tablet-dom 0 '^\[768,1024,2,5\]$' eval '[innerWidth,innerHeight,devicePixelRatio,navigator.maxTouchPoints]'
t preset-tablet-ua 0 "^Mozilla/5.0 \(Linux; Android 14\) AppleWebKit/537.36 \(KHTML, like Gecko\) Chrome/$MAJOR.0.0.0 Safari/537.36,false,Android\$" eval '[navigator.userAgent,navigator.userAgentData.mobile,navigator.userAgentData.platform]+""'
t preset-desktop 0 'viewport 1440x900 landscape dpr 1' viewport desktop
t preset-desktop-dom 0 '^\[1440,900,1,0,false\]$' eval '[innerWidth,innerHeight,devicePixelRatio,navigator.maxTouchPoints,/Android|Headless/.test(navigator.userAgent)]'

# ---------------- (b) story editor: upload, client route, self reload, render fidelity ----------------
t story-mobile 0 'viewport 390x844' viewport 390 844 --mobile --scale 3
t story-nav 0 'Story editor' nav "http://127.0.0.1:$HP/story/create"
t story-clear 0 '' eval 'localStorage.clear(),sessionStorage.clear(),1'
t story-reload0 0 '' reload
t story-no-rotate 0 '^none$' eval 'getComputedStyle(document.getElementById("rot")).display'
python3 -c 'import base64,sys;open(sys.argv[1],"wb").write(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg=="))' "$OUT/pic.png"
n=$(nloads)
t story-upload 0 '' upload '#file' "$OUT/pic.png"
ok story-reloaded "wait_loads $((n+1))" "story editor did not reload itself"
sleep 0.3
t story-editor-route 0 '^/story/editor editor pic.png:[0-9]+$' eval 'location.pathname+" "+document.getElementById("ed").textContent'
chk_load story-after-reload '/story/editor' 390 844 portrait-primary 3 true
t story-text 0 '' text
ok story-no-rotate-text '! printf "%s" "$LAST" | grep -q "Rotate your device"' "editor shows the rotate message in portrait"
t story-webgl 0 '^true$' eval 'window.webgl'
t story-shot 0 'bytes' shot "$OUT/story.png"
python3 - "$OUT/story.png" <<'PY' >"$OUT/png.txt"
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
pos, idat = 8, b''
while pos < len(d):
    n, t = struct.unpack('>I4s', d[pos:pos+8]); body = d[pos+8:pos+8+n]; pos += 12 + n
    if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', body[:10])
    elif t == b'IDAT': idat += body
bpp = {2: 3, 6: 4}[ct]; raw = zlib.decompress(idat); stride = w * bpp
rows, prev, i = [], bytearray(stride), 0
for _ in range(h):
    f = raw[i]; line = bytearray(raw[i+1:i+1+stride]); i += 1 + stride
    for x in range(stride):
        a = line[x-bpp] if x >= bpp else 0; b = prev[x]; c = prev[x-bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    rows.append(line); prev = line
def px(x, y): return tuple(rows[y][x*bpp:x*bpp+3])
print(w, h)
for name, x, y in (('red', 75, 150), ('blue', 225, 150), ('green', 150, 510), ('black', 9, 369), ('white', 900, 150)):
    print(name, *px(x, y))
PY
read -r PW PH <"$OUT/png.txt"
ok story-shot-size '[ "$PW" = 1170 ] && [ "$PH" = 2532 ]' "screenshot is ${PW}x$PH, want 1170x2532"
ok story-canvas2d 'grep -q "^red 255 0 0$" "$OUT/png.txt" && grep -q "^blue 0 0 255$" "$OUT/png.txt"' "canvas2D pixels: $(tr '\n' ' ' <"$OUT/png.txt")"
ok story-webgl-pixels 'grep -q "^green 0 255 0$" "$OUT/png.txt" && grep -q "^black 0 0 0$" "$OUT/png.txt" && grep -q "^white 255 255 255$" "$OUT/png.txt"' "WebGL pixels: $(tr '\n' ' ' <"$OUT/png.txt")"
t story-landscape 0 'landscape' viewport 844 390 --mobile --scale 3
t story-rotate-shown 0 '^block$' eval 'getComputedStyle(document.getElementById("rot")).display'
t story-portrait 0 'portrait' viewport 390 844 --mobile --scale 3
t story-rotate-hidden 0 '^none$' eval 'getComputedStyle(document.getElementById("rot")).display'

# ---------------- (d) other commands do not disturb the keeper's overrides ----------------
t d-nav 0 'Emulation loads' nav "$URL?d=1"
t d-eval 0 '^390$' eval 'innerWidth'
t d-shot 0 'bytes' shot "$OUT/d.png"
t d-snapshot 0 '' snapshot
t d-text 0 '' text
printf '%s\n' '{"id":1,"cmd":"eval","args":["innerWidth"]}' '{"id":2,"cmd":"snapshot"}' >"$OUT/pipe.in"
"$BIN" --session "$SES" pipe <"$OUT/pipe.in" >"$OUT/pipe.out" 2>&1
ok d-pipe 'grep -q "\"value\":390" "$OUT/pipe.out"' "pipe: $(head -c 300 "$OUT/pipe.out")"
python3 - "$BIN" "$SES" "$OUT/mcp.png" >"$OUT/mcp.out" 2>&1 <<'PY'
import json, subprocess, sys
p = subprocess.Popen([sys.argv[1], '--session', sys.argv[2], 'mcp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def rq(i, m, pr):
    p.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': i, 'method': m, 'params': pr}) + '\n').encode()); p.stdin.flush()
    return json.loads(p.stdout.readline())
rq(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 't', 'version': '1'}})
r = rq(2, 'tools/call', {'name': 'eval', 'arguments': {'expression': '[innerWidth,devicePixelRatio]'}})
print(r['result']['content'][0]['text'])
r = rq(3, 'tools/call', {'name': 'screenshot', 'arguments': {'path': sys.argv[3]}})
print('shot', not r['result'].get('isError'), open(sys.argv[3], 'rb').read()[16:24].hex())
p.stdin.close(); p.wait(10)
PY
ok d-mcp 'grep -q "\[390,3\]" "$OUT/mcp.out" && grep -q "shot True 00000492000009e4" "$OUT/mcp.out"' "mcp: $(head -c 300 "$OUT/mcp.out")"
t d-responsive 0 '"count":2' --json responsive "$OUT/resp" --sizes 500x600,700x400
t d-responsive-restored 0 '^\[390,844,3,"portrait-primary"\]$' eval '[innerWidth,innerHeight,devicePixelRatio,screen.orientation.type]'
t d-raw-cdp 0 'keeper_reasserted' --json cdp Emulation.setDeviceMetricsOverride '{"width":500,"height":500,"deviceScaleFactor":1,"mobile":false}'
t d-raw-cdp-restored 0 '^\[390,844,3,"portrait-primary",5\]$' eval '[innerWidth,innerHeight,devicePixelRatio,screen.orientation.type,navigator.maxTouchPoints]'
n=$(nloads)
t d-self-reload 0 '' click '#rl'
ok d-self-reload-done "wait_loads $((n+1))" "no reload"
chk_load d-after-all '?d=1' 390 844 portrait-primary 3 true

# ---------------- (c) JavaScript dialogs never wedge ----------------
t c-arm 0 'armed' eval 'document.getElementById("bu").click(),document.title'
t c-nav-beforeunload 0 'c=1' --timeout 8000 nav "$URL?c=1"
ok c-nav-fast "[ $LASTMS -lt 3000 ]" "nav with beforeunload took $LASTMS ms"
t c-log-beforeunload 0 'beforeunload	accept' dialogs
t c-arm2 0 'armed' eval 'document.getElementById("bu").click(),document.title'
t c-reload-beforeunload 0 'c=1' reload
ok c-reload-fast "[ $LASTMS -lt 3000 ]" "reload with beforeunload took $LASTMS ms"
t c-alert 0 '^after-alert$' eval '(alert("A1"),"after-alert")'
t c-confirm 0 '^false$' eval 'confirm("C1")'
t c-prompt 0 '^null$' eval 'prompt("P1","x")'
t c-log 0 '"type":"alert","message":"A1".*"action":"accept".*"type":"confirm","message":"C1".*"action":"dismiss".*"type":"prompt","message":"P1".*"action":"dismiss"' --json dialogs
t c-policy 0 'beforeunload=accept,alert=accept,confirm=accept,prompt=accept' dialog policy confirm=accept,prompt=accept
t c-confirm-accept 0 '^true$' eval 'confirm("C2")'
t c-prompt-accept 0 '^x$' eval 'prompt("P2","x")'
t c-policy-bad 2 '' dialog policy confirm=maybe
t c-manual 0 'confirm=manual' dialog policy confirm=manual,prompt=manual
t c-open-manual 0 '^1$' eval 'setTimeout(function(){window.r1=confirm("M1")},50),1'
sleep 0.4
t c-blocked-fast 1 'confirm dialog "M1" is open' eval '1+1'
ok c-blocked-fast-ms "[ $LASTMS -lt 2000 ]" "blocked command took $LASTMS ms"
t c-blocked-shot 1 'dialog accept.dismiss' shot "$OUT/blocked.png"
t c-pending 0 'pending: a JavaScript confirm dialog "M1"' dialogs
t c-manual-accept 0 'dialog accepted' dialog accept
t c-manual-result 0 '^true$' eval 'window.r1'
t c-inflight 1 'confirm dialog "M2" is open' eval 'confirm("M2")'
ok c-inflight-fast "[ $LASTMS -lt 3000 ]" "in-flight dialog took $LASTMS ms"
t c-inflight-dismiss 0 'dialog dismissed' dialog dismiss
t c-after-dismiss 0 '^2$' eval '1+1'
t c-prompt-manual 0 '^1$' eval 'setTimeout(function(){window.r2=prompt("M3")},50),1'
sleep 0.4
t c-prompt-text 0 'dialog accepted' dialog accept 'typed text'
t c-prompt-value 0 '^typed text$' eval 'window.r2'
t c-no-dialog 1 'no JavaScript dialog' dialog accept
t c-policy-reset 0 'confirm=dismiss,prompt=dismiss' dialog policy reset
t c-auto 0 '"handled":0' --json --timeout 300 dialog auto accept
t c-log-manual 0 'accept \(manual\)' dialogs --last 50

# ---------------- (e) lifecycle ----------------
ok e-one-keeper '[ "$(keepers "$SES")" = 1 ]' "keepers: $(keepers "$SES")"
"$BIN" --session "$SES" __hold >/dev/null 2>&1
ok e-second-keeper-exits '[ "$(keepers "$SES")" = 1 ]' "a second keeper stayed: $(keepers "$SES")"
KP=$(keeper_pid "$SES")
RSS=$(awk '/^VmRSS/{print $2}' /proc/$KP/status)
T0=$(awk '{print $14+$15}' /proc/$KP/stat); sleep 5; T1=$(awk '{print $14+$15}' /proc/$KP/stat)
echo "   keeper pid $KP: rss ${RSS} KB, cpu $((T1-T0)) ticks in 5 s idle"
ok e-keeper-rss "[ $RSS -lt 4096 ]" "keeper rss $RSS KB"
ok e-keeper-idle "[ $((T1-T0)) -le 2 ]" "keeper used $((T1-T0)) ticks idle"
t e-viewport-lat 0 '' viewport mobile
LK=$(lat "$SES")
# keeper killed -> the next command brings one back, emulation intact
kill "$KP"
for _ in $(seq 30); do kill -0 "$KP" 2>/dev/null || break; sleep 0.1; done
t e-lazy-respawn 0 '^390$' eval 'innerWidth'
ok e-respawned '[ "$(keepers "$SES")" = 1 ] && [ "$(keeper_pid "$SES")" != "$KP" ]' "no new keeper"
n=$(nloads)
t e-respawn-reload 0 '' click '#rl'
ok e-respawn-reload-done "wait_loads $((n+1))" "no reload"
chk_load e-after-respawn 'c=1' 390 844 portrait-primary 3 true
t e-stop 0 'stopped session' stop
ok e-stop-no-keeper '[ "$(keepers "$SES")" = 0 ]' "keeper survived stop"
ok e-stop-no-procs '! pgrep -f -- "$OUT/udd-main" >/dev/null' "browser processes survived stop"

# with the memory governor
ts "$SGOV" g-start 0 'governor pid [0-9]+, keeper pid [0-9]+' start --port $((P0+1)) --user-data-dir "$OUT/udd-gov"
ts "$SGOV" g-viewport 0 'viewport 390x844' viewport mobile
ts "$SGOV" g-nav 0 '' nav "$URL?g=1"
chk_load g-load '?g=1' 390 844 portrait-primary 3 true "$SGOV"
ts "$SGOV" g-stop 0 'stopped' stop
for _ in $(seq 40); do pgrep -f -- "$OUT/udd-gov" >/dev/null || break; sleep 0.1; done
ok g-no-keeper '[ "$(keepers "$SGOV")" = 0 ] && ! pgrep -f -- "$OUT/udd-gov" >/dev/null' "keeper/governor/browser left after stop"

# SLIM_HOLD=0: old per-command re-apply, no keeper; SLIM_KEEP_HEADLESS_UA=1 keeps the engine UA
SLIM_HOLD=0 SLIM_KEEP_HEADLESS_UA=1 "$BIN" --session "$SOLD" start --port $((P0+2)) --user-data-dir "$OUT/udd-old" --no-gov >"$OUT/old.txt" 2>&1
ok o-start '! grep -q keeper "$OUT/old.txt" && [ "$(keepers "$SOLD")" = 0 ]' "SLIM_HOLD=0 start: $(cat "$OUT/old.txt")"
export SLIM_HOLD=0 SLIM_KEEP_HEADLESS_UA=1
ts "$SOLD" o-nav 0 '' nav "$URL?old=1"
ts "$SOLD" o-headless-ua 0 'HeadlessChrome' eval 'navigator.userAgent'
ts "$SOLD" o-viewport 0 'viewport 390x844' viewport 390 844 --mobile --scale 3
ts "$SOLD" o-applied 0 '^\[390,844,"portrait-primary",5\]$' eval '[innerWidth,innerHeight,screen.orientation.type,navigator.maxTouchPoints]'
ok o-no-keeper '[ "$(keepers "$SOLD")" = 0 ]' "SLIM_HOLD=0 spawned a keeper"
LO=$(lat "$SOLD")
ts "$SOLD" o-arm 0 'armed' eval 'document.getElementById("bu").click(),document.title'
ts "$SOLD" o-nav-beforeunload 0 'old=2' --timeout 8000 nav "$URL?old=2"
ok o-nav-fast "[ $LASTMS -lt 3000 ]" "fallback nav with beforeunload took $LASTMS ms"
ts "$SOLD" o-arm2 0 'armed' eval 'document.getElementById("bu").click(),document.title'
ts "$SOLD" o-reload-beforeunload 0 'old=2' --timeout 8000 reload
ok o-reload-fast "[ $LASTMS -lt 3000 ]" "fallback reload with beforeunload took $LASTMS ms"
ts "$SOLD" o-open 0 'old=3' open "$URL?old=3"
ts "$SOLD" o-arm3 0 'armed' eval 'document.getElementById("bu").click(),document.title'
ts "$SOLD" o-close-beforeunload 0 'closed' --timeout 8000 close
ok o-close-fast "[ $LASTMS -lt 4000 ]" "fallback close took $LASTMS ms"
unset SLIM_HOLD SLIM_KEEP_HEADLESS_UA
# stateless --port: no state, no keeper, still never wedges on beforeunload
runs x p-eval 0 --port $((P0+2)) eval 'document.getElementById("bu").click(),document.title' && check p-eval armed
runs x p-nav 0 --port $((P0+2)) --timeout 8000 nav "$URL?port=1" && check p-nav 'port=1'
ok p-nav-fast "[ $LASTMS -lt 3000 ]" "--port nav with beforeunload took $LASTMS ms"
ok p-no-keeper '[ "$(keepers x)" = 0 ]' "--port spawned a keeper"
# attach: an adopted browser gets a keeper too; stop only forgets it
ts "$SATT" a-attach 0 'keeper pid [0-9]+' attach --port $((P0+2))
ts "$SATT" a-viewport 0 'viewport 412x915' viewport 412 915 --mobile --scale 2
ts "$SATT" a-reload 0 '' reload
ts "$SATT" a-dom 0 '^\[412,915,2\]$' eval '[innerWidth,innerHeight,devicePixelRatio]'
ts "$SATT" a-stop 0 'detached' stop
ok a-no-keeper '[ "$(keepers "$SATT")" = 0 ]' "attached keeper survived stop"
SLIM_HOLD=0 "$BIN" --session "$SOLD" stop >/dev/null 2>&1

# keeper exits when the browser dies
ts "$SKILL" k-start 0 'keeper pid' start --port $((P0+3)) --user-data-dir "$OUT/udd-kill" --no-gov
BP=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["pid"])' "$SLIM_STATE_DIR/$SKILL.json")
KK=$(keeper_pid "$SKILL")
kill -9 "$BP"
for _ in $(seq 50); do kill -0 "$KK" 2>/dev/null || break; sleep 0.1; done
ok k-keeper-exits '! kill -0 "$KK" 2>/dev/null' "keeper $KK survived its browser"
pkill -9 -f -- "$OUT/udd-kill" 2>/dev/null
"$BIN" --session "$SKILL" stop >/dev/null 2>&1

echo "   latency of 'eval 1' (avg of 10): keeper $LK ms, SLIM_HOLD=0 re-apply $LO ms"
echo "   slowest command: $MAXNAME $MAXMS ms"
ok never-30s "[ $MAXMS -lt 10000 ]" "a command took $MAXMS ms ($MAXNAME)"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
