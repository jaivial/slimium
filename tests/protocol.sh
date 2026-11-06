#!/usr/bin/env bash
# Live-schema and generic-CDP tests. Uses only CDP 9510 (override within 9510-9529).
set -u
cd "$(dirname "$0")/.."
BIN=${SLIM_BIN:-./slimium}
PORT=${SLIM_TEST_PORT:-9510}
case "$PORT" in 951[0-9]|952[0-9]) ;; *) echo "protocol tests require CDP port 9510-9529" >&2; exit 1;; esac
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required for schema comparison"; exit 1; }
STATE=$(mktemp -d /tmp/slim-protocol-state.XXXXXX)
UDD=$(mktemp -d /tmp/slim-protocol-udd.XXXXXX)
OUT=$(mktemp -d /tmp/slim-protocol-out.XXXXXX)
export SLIM_STATE_DIR="$STATE"
SES=protocol-$$
LISTENER_PID=
DIALOG_PID=
cleanup() {
    [ -z "$LISTENER_PID" ] || kill "$LISTENER_PID" 2>/dev/null
    [ -z "$DIALOG_PID" ] || kill "$DIALOG_PID" 2>/dev/null
    "$BIN" --session "$SES" stop >/dev/null 2>&1
    rm -rf "$STATE" "$UDD" "$OUT"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
pass=0; fail=0; LAST=
run() {
    local name=$1 want=$2 rc; shift 2
    "$BIN" --session "$SES" "$@" >"$OUT/out" 2>"$OUT/err" <"${STDIN:-/dev/null}"
    rc=$?; LAST=$(<"$OUT/out")
    if [ "$rc" != "$want" ]; then
        echo "FAIL [$name] rc=$rc want=$want: $(head -c 400 "$OUT/err") $(head -c 200 "$OUT/out")"
        fail=$((fail+1)); return 1
    fi
    return 0
}
check() {
    if printf '%s' "$LAST" | grep -Eq -- "$2"; then pass=$((pass+1)); else
        echo "FAIL [$1] output did not match /$2/: $(printf '%s' "$LAST" | head -c 400)"; fail=$((fail+1)); fi
}
t() { local n=$1 rc=$2 p=$3; shift 3; if run "$n" "$rc" "$@"; then if [ -n "$p" ]; then check "$n" "$p"; else pass=$((pass+1)); fi; fi; }
ok() { if eval "$2"; then pass=$((pass+1)); else echo "FAIL [$1] $3"; fail=$((fail+1)); fi; }
errcheck() { if grep -Eq -- "$2" "$OUT/err"; then pass=$((pass+1)); else echo "FAIL [$1] stderr: $(head -c 300 "$OUT/err")"; fail=$((fail+1)); fi; }

echo "== slimium protocol tests (CDP $PORT, session $SES)"
t start 0 "started session $SES" start --port "$PORT" --user-data-dir "$UDD" --no-gov || exit 1
python3 - "$PORT" "$OUT/live.json" <<'PY'
import json,sys,urllib.request
with urllib.request.urlopen('http://127.0.0.1:'+sys.argv[1]+'/json/protocol') as r:
    doc=json.load(r)
with open(sys.argv[2],'w') as f: json.dump(doc,f,separators=(',',':'))
PY

# Every live command/event must be listed; JSON retains each complete schema object.
run domains-json 0 --json protocol domains && printf '%s' "$LAST" >"$OUT/domains.json"
python3 - "$OUT/live.json" "$OUT/domains.json" <<'PY'
import json,sys
live=json.load(open(sys.argv[1])); out=json.load(open(sys.argv[2]))
assert out['ok'] and out['domains']==live['domains'], 'domain JSON differs from live schema'
assert out['count']==len(live['domains'])
PY
ok domains-faithful '[ $? = 0 ]' 'domain JSON was not faithful'
run methods-text 0 protocol methods && printf '%s\n' "$LAST" >"$OUT/methods.txt"
python3 - "$OUT/live.json" "$OUT/methods.txt" <<'PY'
import json,sys
live=json.load(open(sys.argv[1]))
want=[d['domain']+'.'+m['name'] for d in live['domains'] for m in d.get('commands',[])]
got=[line.split()[0] for line in open(sys.argv[2]) if line.strip()]
assert got==want, 'method omission/order mismatch: '+repr((set(want)-set(got),set(got)-set(want)))
PY
ok methods-complete '[ $? = 0 ]' 'live methods and protocol methods differ'
run methods-json 0 --json protocol methods && printf '%s' "$LAST" >"$OUT/methods.json"
python3 - "$OUT/live.json" "$OUT/methods.json" <<'PY'
import json,sys
live=json.load(open(sys.argv[1])); out=json.load(open(sys.argv[2]))
want={(d['domain'],m['name']):m for d in live['domains'] for m in d.get('commands',[])}
got={(x['domain'],x['schema']['name']):x['schema'] for x in out['methods']}
assert got==want and out['count']==len(want), 'method JSON differs from live schema'
PY
ok methods-json-faithful '[ $? = 0 ]' 'method JSON differs from live schema'
run events-text 0 protocol events && printf '%s\n' "$LAST" >"$OUT/events.txt"
python3 - "$OUT/live.json" "$OUT/events.txt" <<'PY'
import json,sys
live=json.load(open(sys.argv[1]))
want=[d['domain']+'.'+e['name'] for d in live['domains'] for e in d.get('events',[])]
got=[line.split()[0] for line in open(sys.argv[2]) if line.strip()]
assert got==want, 'event omission/order mismatch'
PY
ok events-complete '[ $? = 0 ]' 'live events and protocol events differ'
run events-json 0 --json protocol events && printf '%s' "$LAST" >"$OUT/events.json"
python3 - "$OUT/live.json" "$OUT/events.json" <<'PY'
import json,sys
live=json.load(open(sys.argv[1])); out=json.load(open(sys.argv[2]))
want={(d['domain'],e['name']):e for d in live['domains'] for e in d.get('events',[])}
got={(x['domain'],x['schema']['name']):x['schema'] for x in out['events']}
assert got==want and out['count']==len(want), 'event JSON differs from live schema'
PY
ok events-json-faithful '[ $? = 0 ]' 'event JSON differs from live schema'

t methods-domain 0 '^Browser\.getVersion' protocol methods Browser
t events-domain 0 '^Runtime\.consoleAPICalled' protocol events Runtime
t domains-text 0 $'^Browser\tmethods=' protocol domains
t version 0 $'^Browser\tChrome/156\.' protocol version
t version-json 0 '"Protocol-Version":"1\.3"' --json protocol version
t show 0 'expression \(required, string\)' protocol show Runtime.evaluate
t show-flags 0 '\[experimental\]' protocol show Browser.grantPermissions
t show-event 0 '\[event\]' protocol show Runtime.consoleAPICalled
run show-json 0 --json protocol show Runtime.evaluate && printf '%s' "$LAST" >"$OUT/show.json"
python3 - "$OUT/show.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x['kind']=='method' and x['schema']['name']=='evaluate'
assert any(p['name']=='expression' and not p.get('optional',False) for p in x['schema']['parameters'])
PY
ok show-required '[ $? = 0 ]' 'required parameter missing from live show schema'
run coverage-json 0 --json protocol coverage && printf '%s' "$LAST" >"$OUT/coverage.json"
python3 - "$OUT/live.json" "$OUT/coverage.json" <<'PY'
import json,sys
live=json.load(open(sys.argv[1])); out=json.load(open(sys.argv[2]))
nm=sum(len(d.get('commands',[])) for d in live['domains'])
ne=sum(len(d.get('events',[])) for d in live['domains'])
assert out['domains']==len(live['domains']) and out['methods']==nm and out['events']==ne
assert out['callable_methods']==nm and out['omitted_methods']==0 and out['coverage_percent']==100
PY
ok coverage-counts '[ $? = 0 ]' 'coverage counts differ from live schema'
t show-unknown 1 '' protocol show Nope.method
errcheck show-unknown 'unknown DevTools domain'
t methods-unknown 1 '' protocol methods Nope
errcheck methods-unknown 'unknown DevTools domain'

# Generic invocation: browser/page, inline/file/stdin, explicit target and session.
t cdp-browser 0 '"product":"(Headless)?Chrome/156\.' cdp Browser.getVersion
t cdp-inline 0 '"value":42' cdp Runtime.evaluate '{"expression":"6*7","returnByValue":true}'
printf '%s' '{"expression":"40+2","returnByValue":true}' >"$OUT/params.json"
t cdp-file 0 '"value":42' cdp Runtime.evaluate "@$OUT/params.json"
printf '%s' '{"expression":"21*2","returnByValue":true}' >"$OUT/stdin.json"
STDIN="$OUT/stdin.json" t cdp-stdin 0 '"value":42' cdp Runtime.evaluate -
t cdp-bad-inline 2 '' cdp Runtime.evaluate '{bad'
printf '%s' '{"expression":' >"$OUT/invalid.json"
t cdp-bad-file 2 '' cdp Runtime.evaluate "@$OUT/invalid.json"
errcheck cdp-bad-file 'PARAMS must be a JSON object'
t cdp-unknown 1 '' cdp Nope.method
errcheck cdp-unknown "Nope.method.*wasn't found|not found"
TID=$("$BIN" --session "$SES" cdp Target.getTargets | python3 -c 'import json,sys; print(next(t["targetId"] for t in json.load(sys.stdin)["targetInfos"] if t["type"]=="page"))')
t cdp-target-id 0 '"value":42' cdp Runtime.evaluate '{"expression":"42","returnByValue":true}' --target-id "${TID:0:12}"
run cdp-attach 0 --json cdp Target.attachToTarget "{\"targetId\":\"$TID\",\"flatten\":true}" --target browser
SID=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["sessionId"])')
# A flat session from another CLI connection is stale after that connection closes.
t cdp-session-stale 1 '' cdp Runtime.evaluate '{"expression":"42","returnByValue":true}' --cdp-session "$SID"
errcheck cdp-session-stale 'Session with given id not found'
t cdp-route-conflict 2 '' cdp Browser.getVersion --target browser --target-id "$TID"

# A result larger than any legacy fixed buffer remains complete and valid JSON.
run cdp-large 0 --json cdp Runtime.evaluate '{"expression":"\"x\".repeat(300000)","returnByValue":true}'
printf '%s' "$LAST" >"$OUT/large.json"
python3 - "$OUT/large.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x['result']['result']['value']=='x'*300000
PY
ok cdp-large-complete '[ $? = 0 ]' 'large CDP result was truncated'

# Event listener: Runtime.enable is automatic; trigger from a second connection.
"$BIN" --session "$SES" --json --timeout 5000 cdp-listen Runtime.consoleAPICalled --count 1 >"$OUT/listen.json" 2>"$OUT/listen.err" &
LISTENER_PID=$!
sleep 0.3
t listener-trigger 0 'listener-ok' eval 'console.log("listener-ok"); "listener-ok"'
wait "$LISTENER_PID"; LRC=$?; LISTENER_PID=
LAST=$(<"$OUT/listen.json")
ok listener-rc '[ "$LRC" = 0 ]' "listener failed: $(<"$OUT/listen.err")"
check listener-event '"method":"Runtime\.consoleAPICalled"'
check listener-param 'listener-ok'
t listener-timeout 0 '"count":0' --json --timeout 30 cdp-listen Runtime.exceptionThrown --no-enable

# Thin wrappers, each implemented by the documented real CDP method.
t permissions-grant 0 'permissions granted' permissions grant geolocation --origin https://example.com
t permissions-reset 0 'permissions reset' permissions reset
t permissions-reset-origin 2 '' permissions reset --origin https://example.com
t geolocation-set 0 'geolocation set' geolocation set 40.4168 -3.7038 25
t geolocation-clear 0 'geolocation cleared' geolocation clear
t geolocation-bad 2 '' geolocation set 100 20
t throttle-slow 0 'network throttling set' throttle slow-3g
t throttle-off 0 'network throttling off' throttle off
t throttle-bad 2 '' throttle impossible
t cache-off 0 'cache disabled' cache off
t cache-on 0 'cache enabled' cache on
t script-off 0 'page scripts disabled' script off
t script-on 0 'page scripts enabled' script on

# Auto dialog handling stays alive to receive Page.javascriptDialogOpening.
"$BIN" --session "$SES" --json --timeout 5000 dialog auto accept >"$OUT/dialog.json" 2>"$OUT/dialog.err" &
DIALOG_PID=$!
sleep 0.3
t dialog-trigger 0 'after-dialog' eval '(alert("protocol-test"),"after-dialog")'
wait "$DIALOG_PID"; DRC=$?; DIALOG_PID=
LAST=$(<"$OUT/dialog.json")
ok dialog-rc '[ "$DRC" = 0 ]' "dialog handler failed: $(<"$OUT/dialog.err")"
check dialog-handled '"handled":1,"accept":true'

# Report the exact live counts for the engine under test.
python3 - "$OUT/live.json" "$PORT" <<'PY' >"$OUT/counts"
import json,sys,urllib.request
p=json.load(open(sys.argv[1]))
with urllib.request.urlopen('http://127.0.0.1:'+sys.argv[2]+'/json/version') as r: v=json.load(r)
print('== %s: %d domains, %d methods, %d events; 0 listing omissions' % (
 v['Browser'],len(p['domains']),sum(len(d.get('commands',[])) for d in p['domains']),
 sum(len(d.get('events',[])) for d in p['domains'])))
PY
cat "$OUT/counts"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
