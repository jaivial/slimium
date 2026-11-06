#!/usr/bin/env bash
# Media track integration test. Isolated session, CDP 9490-9509, fresh profiles only.
set -u
cd "$(dirname "$0")/.."
BIN=$(printenv SLIM_BIN || true); [ -n "$BIN" ] || BIN=./slimium
PORT=$(printenv SLIM_TEST_PORT || true); [ -n "$PORT" ] || PORT=9490
KEEP=$(printenv SLIM_MEDIA_KEEP || true)
case "$PORT" in ''|*[!0-9]*) echo "bad SLIM_TEST_PORT"; exit 2;; esac
[ "$PORT" -ge 9490 ] && [ "$PORT" -le 9509 ] || { echo "media tests require CDP 9490-9509"; exit 2; }
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg required"; exit 1; }
command -v ffprobe >/dev/null || { echo "ffprobe required"; exit 1; }
command -v python3 >/dev/null || { echo "python3 required for JSON assertions"; exit 1; }
STATE=$(mktemp -d /tmp/slim-media-test-state.XXXXXX)
UDD=$(mktemp -d /tmp/slim-media-test-udd.XXXXXX)
OUT=$(mktemp -d /tmp/slim-media-test-out.XXXXXX)
SES=media$$
export SLIM_STATE_DIR="$STATE"
FIXTURE="$PWD/tests/fixtures/media-animated.html"
pass=0; fail=0; LAST=; STOPPED=0
: >"$OUT/metrics.tsv"
cleanup() {
    if [ "$STOPPED" = 0 ]; then
        ids=$(python3 - "$STATE" "$SES" 2>/dev/null <<'PY'
import glob,json,sys
for p in glob.glob(sys.argv[1]+'/record-'+sys.argv[2]+'-*.json'):
    try:
        d=json.load(open(p))
        if d.get('status') in ('starting','recording'): print(d['id'])
    except Exception: pass
PY
)
        for id in $ids; do "$BIN" --session "$SES" record stop --id "$id" >/dev/null 2>&1; done
        "$BIN" --session "$SES" stop >/dev/null 2>&1
    fi
    if [ "$KEEP" = 1 ]; then echo "media artifacts: $OUT"; else rm -rf "$OUT"; fi
    rm -rf "$UDD" "$STATE"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
run() {
    local name=$1 want=$2; shift 2
    local t0=$(date +%s%N) t1 rc
    if [ -x /usr/bin/time ]; then
        /usr/bin/time -f '%M' -o "$OUT/time" "$BIN" --session "$SES" "$@" >"$OUT/stdout" 2>"$OUT/stderr" </dev/null
    else
        "$BIN" --session "$SES" "$@" >"$OUT/stdout" 2>"$OUT/stderr" </dev/null
    fi
    rc=$?; t1=$(date +%s%N); LAST=$(cat "$OUT/stdout")
    local rss=-; [ -s "$OUT/time" ] && rss=$(tail -1 "$OUT/time")
    printf '%s\t%d\t%s\n' "$name" $(((t1-t0)/1000000)) "$rss" >>"$OUT/metrics.tsv"
    if [ "$rc" != "$want" ]; then
        echo "FAIL [$name] rc=$rc want=$want: $(head -c 300 "$OUT/stderr") $(head -c 200 "$OUT/stdout")"
        fail=$((fail+1)); return 1
    fi
    return 0
}
check() { if printf '%s' "$LAST" | grep -Eq -- "$2"; then pass=$((pass+1)); else echo "FAIL [$1] wanted /$2/: $(printf '%s' "$LAST" | head -c 300)"; fail=$((fail+1)); fi; }
t() { local n=$1 rc=$2 p=$3; shift 3; if run "$n" "$rc" "$@"; then if [ -n "$p" ]; then check "$n" "$p"; else pass=$((pass+1)); fi; fi; }
ok() { if eval "$2"; then pass=$((pass+1)); else echo "FAIL [$1] $3"; fail=$((fail+1)); fi; }
pngw() { od -An -tu1 -j16 -N4 "$1" | awk '{print $1*16777216+$2*65536+$3*256+$4}'; }
pngh() { od -An -tu1 -j20 -N4 "$1" | awk '{print $1*16777216+$2*65536+$3*256+$4}'; }
json() { printf '%s' "$LAST" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[sys.argv[1]])' "$1"; }
probe() { ffprobe -v error -show_entries "$2" -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null; }

echo "== slimium media tests (port $PORT, session $SES)"
t record-usage 2 '' record
t start 0 'started session' start --port "$PORT" --user-data-dir "$UDD" --no-gov --window 1000x700 || { echo 'cannot start test browser'; exit 1; }
t nav 0 'Media animation fixture' nav "$FIXTURE"
t sentinel 0 '^media-alive$' eval 'window.mediaSentinel="media-alive"'
t viewport 0 'viewport 800x600' viewport 800 600
t viewport-dom 0 '^\[800,600\]$' eval '[innerWidth,innerHeight]'
t viewport-no-reload 0 '^media-alive$' eval 'window.mediaSentinel'
t viewport-css-tablet 0 'tablet' eval 'getComputedStyle(document.querySelector("#break"),"::after").content'
t resize-alias 0 'viewport 900x500' resize 900 500
t resize-dom 0 '^\[900,500\]$' eval '[innerWidth,innerHeight]'
t preset-mobile 0 'viewport 375x812' viewport mobile
t preset-mobile-dom 0 '^\[375,812\]$' eval '[innerWidth,innerHeight]'
t preset-mobile-css 0 'mobile' eval 'getComputedStyle(document.querySelector("#break"),"::after").content'
t preset-tablet 0 'viewport 768x1024' viewport tablet
t preset-tablet-css 0 'tablet' eval 'getComputedStyle(document.querySelector("#break"),"::after").content'
t preset-desktop 0 'viewport 1440x900' viewport desktop
t preset-desktop-css 0 'desktop' eval 'getComputedStyle(document.querySelector("#break"),"::after").content'
t viewport-record-base 0 'viewport 800x600' viewport 800 600

t responsive 0 '"count":3' --json responsive "$OUT/responsive" --sizes 375x812,768x1024,1440x900
for size in 375x812 768x1024 1440x900; do
    w=$(printf '%s' "$size" | cut -dx -f1); h=$(printf '%s' "$size" | cut -dx -f2)
    ok "responsive-$size" '[ "$(pngw "$OUT/responsive/$size.png")" = "$w" ] && [ "$(pngh "$OUT/responsive/$size.png")" = "$h" ]' "bad dimensions for $size.png"
done
t responsive-restored 0 '^\[800,600\]$' eval '[innerWidth,innerHeight]'
t responsive-no-reload 0 '^media-alive$' eval 'window.mediaSentinel'

t shot-png 0 '"format":"png"' --json shot "$OUT/shot.png"
ok shot-png-size '[ "$(pngw "$OUT/shot.png")" = 800 ] && [ "$(pngh "$OUT/shot.png")" = 600 ]' 'PNG is not 800x600'
t shot-jpeg 0 '"format":"jpeg"' --json shot "$OUT/shot.jpg" --quality 70
LAST=$(od -An -tx1 -N3 "$OUT/shot.jpg" | tr -d ' '); check shot-jpeg-magic '^ffd8ff'
t shot-webp 0 '"format":"webp"' --json shot "$OUT/shot.webp"
LAST=$(head -c 12 "$OUT/shot.webp" | tail -c 4); check shot-webp-magic 'WEBP'

before=$(ls "$STATE"/record-*.json 2>/dev/null | wc -l)
t start-bad-tab 1 '' --tab NO_SUCH_TARGET record start "$OUT/never.mp4"
after=$(ls "$STATE"/record-*.json 2>/dev/null | wc -l)
ok start-failure-no-worker '[ "$before" = "$after" ]' "metadata/worker created for failed attach"

t record-mp4-start 0 '"status":"recording"' --json record start "$OUT/animated.mp4" --fps 10 --quality 70
RID=$(json id)
sleep 1.2
t record-survives-title 0 'Media animation fixture' title
t record-survives-eval 0 '^[0-9]+$' eval 'window.mediaFrame'
t record-resize 0 'viewport 900x500' viewport 900 500
t record-resize-dom 0 '^\[900,500\]$' eval '[innerWidth,innerHeight]'
t record-shot-during 0 '"format":"png"' --json shot "$OUT/during.png"
ok record-shot-size '[ "$(pngw "$OUT/during.png")" = 900 ] && [ "$(pngh "$OUT/during.png")" = 500 ]' 'recording-time screenshot is not 900x500'
sleep 2
t record-status 0 '"alive":true' --json record status
t record-mp4-stop 0 '"status":"stopped"' --json record stop --id "$RID"
DUR=$(probe "$OUT/animated.mp4" format=duration)
FRAMES=$(probe "$OUT/animated.mp4" stream=nb_frames | head -1)
CODEC=$(probe "$OUT/animated.mp4" stream=codec_name | head -1)
STREAMS=$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$OUT/animated.mp4" | sort | tr '\n' ' ')
ok record-mp4-duration 'python3 -c "import sys; sys.exit(float(sys.argv[1]) < 3.0)" "$DUR"' "duration $DUR < 3s"
ok record-mp4-frames '[ "$FRAMES" -ge 30 ]' "frames $FRAMES < 30"
ok record-mp4-streams '[ "$CODEC" = h264 ] && [ "$STREAMS" = "video " ]' "unexpected streams: $CODEC $STREAMS"
SIZE=$(stat -c %s "$OUT/animated.mp4")
ok record-mp4-size '[ "$SIZE" -gt 1000 ]' "output too small: $SIZE"
ffmpeg -hide_banner -loglevel error -y -i "$OUT/animated.mp4" -vf 'select=eq(n\,0)' -frames:v 1 "$OUT/first.png"
ffmpeg -hide_banner -loglevel error -y -i "$OUT/animated.mp4" -vf "select=eq(n\,$((FRAMES-1)))" -frames:v 1 "$OUT/last.png"
H1=$(ffmpeg -hide_banner -loglevel error -i "$OUT/first.png" -f rawvideo -pix_fmt rgba - 2>/dev/null | sha256sum | cut -d' ' -f1)
H2=$(ffmpeg -hide_banner -loglevel error -i "$OUT/last.png" -f rawvideo -pix_fmt rgba - 2>/dev/null | sha256sum | cut -d' ' -f1)
ok record-visual-change '[ -n "$H1" ] && [ -n "$H2" ] && [ "$H1" != "$H2" ]' "first/last pixels missing or identical ($H1 $H2)"
t record-list 0 '"status":"stopped"' --json record list
t record-status-empty 0 '"count":0' --json record status

t record-webm-start 0 '"format":"webm"' --json record start "$OUT/auto.webm" --fps 8 --quality 60 --max-seconds 2
sleep 3
t record-webm-auto 0 '"status":"stopped"' --json record list
WDUR=$(probe "$OUT/auto.webm" format=duration)
WCODEC=$(probe "$OUT/auto.webm" stream=codec_name | head -1)
ok record-webm-stream '[ "$WCODEC" = vp9 ]' "unexpected WebM codec $WCODEC"
ok record-webm-duration 'python3 -c "import sys; d=float(sys.argv[1]); sys.exit(not (1.5 <= d <= 3.5))" "$WDUR"' "bad WebM duration $WDUR"

t record-avi-start 0 '"format":"avi"' --json record start "$OUT/clip.avi" --fps 8 --quality 60
sleep 1
t record-avi-stop 0 '"status":"stopped"' --json record stop
ACODEC=$(probe "$OUT/clip.avi" stream=codec_name | head -1)
ADUR=$(probe "$OUT/clip.avi" format=duration)
ok record-avi-stream '[ "$ACODEC" = mpeg4 ]' "unexpected AVI codec $ACODEC"
ok record-avi-duration 'python3 -c "import sys; sys.exit(float(sys.argv[1]) < .8)" "$ADUR"' "bad AVI duration $ADUR"

t static-nav 0 'Static$' nav 'data:text/html,<title>Static</title><p>unchanged</p>'
t static-record 0 '"status":"recording"' --json record start "$OUT/static.mp4" --fps 5 --max-seconds 1
sleep 2
SFRAMES=$(probe "$OUT/static.mp4" stream=nb_frames | head -1)
ok static-duplicate-frames '[ "$SFRAMES" -ge 5 ]' "static page produced only $SFRAMES frames"
t animated-again 0 'Media animation fixture' nav "$FIXTURE"

t death-record 0 '"status":"recording"' --json record start "$OUT/browser-death.mp4" --fps 8
DID=$(json id)
DPID=$(json pid)
sleep 1
t browser-stop 0 'stopped session' stop
STOPPED=1
META="$STATE/record-$SES-$DID.json"
DSTATUS=starting
for i in $(seq 1 150); do
    [ -s "$META" ] && DSTATUS=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "$META" 2>/dev/null)
    { [ "$DSTATUS" = stopped ] || [ "$DSTATUS" = error ]; } && break
    sleep .1
done
ok death-finalized '[ "$DSTATUS" = stopped ]' "browser death left status $DSTATUS"
DDUR=$(probe "$OUT/browser-death.mp4" format=duration)
ok death-video-valid 'python3 -c "import sys; sys.exit(float(sys.argv[1]) < .5)" "$DDUR"' "invalid finalized browser-death video: $DDUR"
ok death-worker-reaped '! kill -0 "$DPID" 2>/dev/null || [ "$(ps -o stat= -p "$DPID" 2>/dev/null | tr -d " ")" = Z ]' "recorder $DPID still running"
python3 - "$STATE" <<'PY'
import glob,json,os,sys
bad=[]
for p in glob.glob(sys.argv[1]+'/record-*.json'):
    d=json.load(open(p))
    if d.get('ffmpeg_pid'): bad.append((d['id'],'ffmpeg still registered'))
    pid=d.get('pid',0)
    if pid and os.path.exists('/proc/%d/stat'%pid):
        stat=open('/proc/%d/stat'%pid).read().rsplit(')',1)[1].split()[0]
        if stat!='Z': bad.append((d['id'],'worker still running'))
if bad:
    print('FAIL [no-leftover-media-processes]',bad)
    sys.exit(1)
print('no leftover recorder/ffmpeg processes')
PY
ok no-leftover-media-processes '[ "$?" = 0 ]' 'recorded worker/ffmpeg process remains'

cp "$OUT/metrics.tsv" /tmp/slimium-media-metrics.tsv
awk -F'\t' '$3!="-"{n++;s+=$2;if($3>m){m=$3;mc=$1}}END{printf "== %d timed media commands: avg %.1f ms, peak CLI RSS %d KB (%s)\n",n,s/n,m,mc}' "$OUT/metrics.tsv"
echo "== MP4: $DUR seconds, $FRAMES frames, $SIZE bytes; WebM: $WDUR seconds; AVI: $ADUR seconds"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
