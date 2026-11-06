#!/usr/bin/env bash
# Interaction track: locators, refs, frames, forms, act and NDJSON pipe.
# Uses only CDP 9470 and fixture HTTP 9488 by default, with fresh /tmp state.
set -u
cd "$(dirname "$0")/.."
BIN=${SLIM_BIN:-./slimium}
PORT=${SLIM_INTERACT_PORT:-9470}
HPORT=${SLIM_INTERACT_HTTP_PORT:-9488}
[ -x "$BIN" ] || { echo "missing $BIN (run make)"; exit 1; }
STATE=$(mktemp -d /tmp/slim-interact-test-state.XXXXXX)
UDD=$(mktemp -d /tmp/slim-interact-test-udd.XXXXXX)
OUT=$(mktemp -d /tmp/slim-interact-test-out.XXXXXX)
SES=interaction-$$
HPID=
cleanup() {
    [ -n "$HPID" ] && kill "$HPID" 2>/dev/null
    SLIM_STATE_DIR="$STATE" "$BIN" --session "$SES" stop >/dev/null 2>&1
    rm -rf "$UDD" "$OUT" "$STATE"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
export SLIM_STATE_DIR="$STATE"
python3 -m http.server --bind 127.0.0.1 "$HPORT" --directory tests/fixtures >"$OUT/http.log" 2>&1 &
HPID=$!
pass=0; fail=0; LAST=
run() {
    local name=$1 want=$2; shift 2
    "$BIN" --session "$SES" "$@" >"$OUT/out" 2>"$OUT/err"
    local rc=$?
    LAST=$(cat "$OUT/out")
    if [ "$rc" != "$want" ]; then
        echo "FAIL [$name] rc=$rc want=$want: $(head -c 400 "$OUT/err") $(head -c 300 "$OUT/out")"
        fail=$((fail+1)); return 1
    fi
    return 0
}
check() {
    if printf '%s' "$LAST" | grep -Eq -- "$2"; then pass=$((pass+1)); else
        echo "FAIL [$1] no match /$2/: $(printf '%s' "$LAST" | head -c 400)"; fail=$((fail+1)); fi
}
errcheck() {
    if grep -Eq -- "$2" "$OUT/err"; then pass=$((pass+1)); else
        echo "FAIL [$1] stderr no match /$2/: $(head -c 400 "$OUT/err")"; fail=$((fail+1)); fi
}
t() { local n=$1 rc=$2 p=$3; shift 3; if run "$n" "$rc" "$@"; then if [ -n "$p" ]; then check "$n" "$p"; else pass=$((pass+1)); fi; fi; }
ok() { if eval "$2"; then pass=$((pass+1)); else echo "FAIL [$1] $3"; fail=$((fail+1)); fi; }
jref() { python3 -c 'import json,sys; a=json.load(open(sys.argv[1]))["elements"]; print(a[int(sys.argv[2])]["ref"])' "$1" "$2"; }
URL="http://127.0.0.1:$HPORT/interact.html"

echo "== slimium interaction tests (CDP $PORT, HTTP $HPORT) =="
t help 0 'snapshot|find|pipe' help
run start 0 start --port "$PORT" --user-data-dir "$UDD" --no-gov --window 1000x700 || { echo "cannot start interaction browser"; exit 1; }
check start 'started session'
t nav 0 'Interaction fixture' nav "$URL"
t snapshot 0 '@e[0-9]+ button "Save"' snapshot --interactive
run snapshot-json 0 --json snapshot --interactive --max 5
check snapshot-json '"count":5'
ok snapshot-bounds "SNAP='$OUT/out' python3 -c 'import json,os;d=json.load(open(os.environ[\"SNAP\"]));assert len(d[\"elements\"])==5 and d[\"truncated\"]'" "max/truncated not bounded"
ok password-json "! grep -q DO_NOT_DISCLOSE "$OUT/out"" "password value disclosed in JSON snapshot"
t snapshot-text 0 'value=<redacted>' snapshot --interactive
ok password-text "! grep -q DO_NOT_DISCLOSE "$OUT/out"" "password value disclosed in text snapshot"
t snapshot-scope 0 'frame=0' snapshot --selector '#myform' --max 30
ok snapshot-scope-no-frame "! grep -q 'frame=0/' "$OUT/out"" "selector escaped its scope"
t snapshot-all 0 'Hidden action.*visible=false' snapshot --all --max 100
ok state-mode '[ "$(stat -c %a "$STATE/$SES.interact.json")" = 600 ] && [ "$(stat -c %a "$STATE")" = 700 ]' "ref state is not 0600/0700"
ok state-no-password "! grep -q DO_NOT_DISCLOSE "$STATE/$SES.interact.json"" "password persisted in ref metadata"

t find-role 0 '^@e[0-9]+ button "Save"' find 'role=button[name="Save"]'
SAVE=$(cut -d' ' -f1 "$OUT/out")
t click-ref 0 "clicked $SAVE" click "$SAVE"
t click-ref-effect 0 '^saved 1$' text '#result'
t find-testid 0 'User name' find testid=username
NAME=$(cut -d' ' -f1 "$OUT/out")
t find-ref 0 "^$NAME " find "$NAME"
t inspect-ref 0 "^#name" inspect "$NAME"
t fill-testid 0 'filled testid=username' fill testid=username Alice
t fill-label 0 'filled label=Biography' fill label=Biography 'hello world'
t fill-xpath 0 'filled xpath=' fill 'xpath=//textarea[@id="bio"]' 'xpath text'
t fill-css 0 'filled css=' fill 'css=#bio' 'css text'
t check-role 0 'checked' check role=checkbox
t select-label 0 'selected g' select label=Color Green
run form-values 0 --json eval '({name:document.querySelector("#name").value,bio:document.querySelector("#bio").value,checked:document.querySelector("#agree").checked,color:document.querySelector("#color").value})'
check form-values '"name":"Alice".*"bio":"css text".*"checked":true.*"color":"g"'
t text-exact-shadow 0 'Shadow save' find 'text="Shadow save"'
SHADOW=$(cut -d' ' -f1 "$OUT/out")
t click-shadow 0 'clicked' click "$SHADOW"
t shadow-effect 0 'shadow clicked' text '#result'
t text-substring 0 'Shadow save' find text=Shadow
t inspect-json 0 '"selector":"#secret"' --json inspect label=Secret
ok inspect-password "! grep -q DO_NOT_DISCLOSE "$OUT/out"" "password attribute disclosed by inspect"
check inspect-styles '"styles":{.*"display"'

run frame-find 0 --json find 'role=button[name="Frame save"]'
check frame-find '"count":2'
cp "$OUT/out" "$OUT/frames.json"
B0=$(jref "$OUT/frames.json" 0); B1=$(jref "$OUT/frames.json" 1)
run frame-results 0 --json find 'text="frame ready"'
cp "$OUT/out" "$OUT/frame-results.json"
R0=$(jref "$OUT/frame-results.json" 0); R1=$(jref "$OUT/frame-results.json" 1)
t click-same-frame 0 'clicked' click "$B0"
t click-cross-frame 0 'clicked' click "$B1"
t same-frame-effect 0 'frame clicked' text "$R0"
t cross-frame-effect 0 'frame clicked' text "$R1"

t dynamic-re-nav 0 'Interaction fixture' nav "$URL"
t wait-delayed 0 'found' wait 'text="Delayed"' --visible --timeout 2000
t click-delayed 0 'clicked' click 'text="Delayed"'
t delayed-effect 0 'delayed clicked' text '#result'
t hidden-wait 1 '' wait '#hidden' --visible --timeout 200
errcheck hidden-wait 'timeout'
t hidden-click 1 '' click '#hidden'
errcheck hidden-click 'not visible'

python3 - <<PY
import json
json.dump({'testid=username':'Dora','label=Agree terms':True,'label=Color':'Green','label=Biography':'form fill'},open('$OUT/form.json','w'))
PY
t form-fill 0 'filled 4 fields' form fill "@$OUT/form.json"
t form-fill-effect 0 '"name":"Dora".*"bio":"form fill".*"checked":true.*"color":"g"' --json eval '({name:document.querySelector("#name").value,bio:document.querySelector("#bio").value,checked:document.querySelector("#agree").checked,color:document.querySelector("#color").value})'
printf one >"$OUT/a.txt"; printf two >"$OUT/b.txt"
t upload 0 'uploaded 2 files' upload label=Attachments "$OUT/a.txt" "$OUT/b.txt"
t upload-effect 0 'files 2' text '#result'
t upload-wrong 1 '' upload testid=username "$OUT/a.txt"
t drag 0 'dragged' drag '#drag' '#drop'
t drag-effect 0 'dropped' text '#result'
t focus 0 'focused' focus testid=username
t focus-effect 0 '^name$' eval 'document.activeElement.id'
t submit 0 'submitted' submit 'role=button[name="Submit form"]'
t submit-effect 0 'submitted' text '#result'

python3 - <<PY
import json
json.dump([
 {'action':'click','locator':'role=button[name="Save"]'},
 {'action':'click','locator':'#missing-act'},
 {'action':'eval','js':'document.querySelector("#result").textContent'},
 {'action':'snapshot','interactive':True,'max':3}
],open('$OUT/act.json','w'))
json.dump([{'action':'click','locator':'#missing-act'},{'action':'eval','js':'1+1'}],open('$OUT/act-stop.json','w'))
PY
t act 0 '"actions":4,"failures":1' --timeout 500 --json act "@$OUT/act.json"
check act-index '"index":1,"action":"click","ok":false,"error"'
check act-continue '"index":2,"action":"eval","ok":true,"value":"saved 1"'
t act-stop 0 '"actions":1,"failures":1' --timeout 300 --json act --stop-on-error "@$OUT/act-stop.json"
t act-final-snapshot 0 '"snapshot":{"count":' --json act --snapshot '[{"action":"eval","js":"1+2"}]'
t act-malformed 1 '' act '{}'
t act-recursive 0 '"failures":1' --json act '[{"action":"pipe"}]'

t pipe-recursion 0 '"ok":false.*recursive pipe' pipe <<<'{"id":9,"cmd":"pipe"}'
TAB=$("$BIN" --session "$SES" --json tabs | python3 -c 'import json,sys;print(next(x["id"] for x in json.load(sys.stdin)["tabs"] if x["current"]))')
export TEST_BIN="$BIN" TEST_SES="$SES" TAB
if python3 - <<'PY'
import json,os,subprocess
p=subprocess.Popen([os.environ['TEST_BIN'],'--session',os.environ['TEST_SES'],'pipe'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
def req(x):
    p.stdin.write(json.dumps(x,separators=(',',':'))+'\n');p.stdin.flush()
    return json.loads(p.stdout.readline())
p.stdin.write('not json\n');p.stdin.flush();assert json.loads(p.stdout.readline())['ok'] is False
before_response=req({'cmd':'eval','args':['window.clicks||0']});assert before_response.get('value') is not None,before_response;before=before_response['value']
assert req({'cmd':'click','args':['#save']})['ok']
after_response=req({'cmd':'eval','args':['window.clicks||0']});assert after_response.get('value') is not None,after_response;after=after_response['value'];assert after==before+1,(before,after,after_response)
fill_response=req({'cmd':'fill','args':['#name','Pipeline']});assert fill_response.get('ok'),fill_response
value_response=req({'cmd':'eval','args':['document.querySelector("#name").value']});assert value_response.get('value')=='Pipeline',value_response
for i in range(100):
    d=req({'id':i,'cmd':'eval','args':[str(i)+'+1']})
    assert d['id']==i and d['ok'] and d['value']==i+1,(i,d)
d=req({'id':'argv','argv':['eval','6*7','--timeout','1000']});assert d['value']==42,d
d=req({'id':'tab','cmd':'eval','args':['location.href','--tab',os.environ['TAB']]});assert d['ok'],d
d=req({'cmd':'pipe'});assert not d['ok'] and 'recursive' in d['error'],d
p.stdin.close();assert p.wait(timeout=5)==0
PY
then
    pass=$((pass+1))
else
    echo "FAIL [pipe-stream] 100-request NDJSON streaming/flush/options failed"
    fail=$((fail+1))
fi

t stale-setup 0 'Interaction fixture' nav "$URL"
t stale-find 0 'Save' find 'role=button[name="Save"]'
STALE=$(cut -d' ' -f1 "$OUT/out")
t stale-nav 0 'Next fixture' nav "http://127.0.0.1:$HPORT/interact-next.html"
t stale-click 1 '' click "$STALE"
errcheck stale-click 'stale ref'
t hash-setup 0 'Interaction fixture' nav "$URL"
t hash-find 0 'Save' find 'role=button[name="Save"]'
STALE=$(cut -d' ' -f1 "$OUT/out")
t hash-nav 0 'Interaction fixture' nav "$URL#changed"
t hash-stale 1 '' click "$STALE"
errcheck hash-stale 'stale ref.*navigation'
t detach-find 0 'Save' find 'role=button[name="Save"]'
STALE=$(cut -d' ' -f1 "$OUT/out")
t detach-node 0 'true' eval 'document.querySelector("#save").remove();true'
t detach-stale 1 '' click "$STALE"
errcheck detach-stale 'stale ref.*attached'
t render 0 'png' shot "$OUT/interact.png"
ok render-png "PNG='$OUT/interact.png' python3 -c 'import struct,os;assert struct.unpack(\">I\",open(os.environ[\"PNG\"],\"rb\").read()[16:20])[0]==1000'" "viewport rendering width changed"

echo "== interaction: $pass passed, $fail failed =="
[ "$fail" = 0 ]
