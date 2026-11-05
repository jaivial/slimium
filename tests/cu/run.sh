#!/usr/bin/env bash
# tests/cu/run.sh -- prove cu works fully on slimium, through the cu-slim CLI.
#
# Starts its OWN isolated cu-slim daemon (temp data dir, HTTP 8792, CDP 9392)
# on slimium-chrome (the real wrapper if ~/.local/bin/slimium-chrome exists,
# else tests/cu/slimium-chrome-standin.sh), serves tests/cu/fixtures on
# 127.0.0.1:8791, drives cu's feature surface and asserts every answer. Stops
# only what it started (trap). Checks the user's own cu is untouched before and
# after. Exit 0 = all checks passed.
#
#   tests/cu/run.sh            # all checks
#   PARALLEL=50 tests/cu/run.sh
#   CU_BENCH=1 tests/cu/run.sh # also cu's own bench/parallel_tabs.py on slimium
#   CU_SLIM_TEST_BROWSER=/path/to/browser tests/cu/run.sh  # another browser
set -u
cd "$(dirname "$0")" || exit 2
source ./lib.sh

FIX_PORT=${FIX_PORT:-8791}
HTTP_PORT=${HTTP_PORT:-8792}
CDP_PORT=${CDP_PORT:-9392}
PARALLEL=${PARALLEL:-20}
BROWSER=${CU_SLIM_TEST_BROWSER:-$(slim_browser)}
BASE=http://127.0.0.1:$FIX_PORT
H=$(mktemp -d /tmp/cu-slim-test.XXXXXX)
LOG=$H/run.log
export CU_SLIM_HOME=$H
pass=0 fail=0 failed=()

cleanup() {
  stop_instance T
  stop_fixtures
  [ -n "${KEEP:-}" ] || rm -rf "${H:?}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ok()   { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); failed+=("$1"); printf 'FAIL %s: %s\n' "$1" "$(printf '%s' "$2" | tr '\n' ' ' | cut -c1-400)"; }
# expect NAME ERE TEXT: TEXT matches ERE.
expect() { if printf '%s' "$3" | grep -Eq -- "$2"; then ok "$1"; else bad "$1" "$3"; fi; }
# known NAME ERE TEXT: a check that fails the same way on stock Chromium (a cu
# issue, not a slimium one): reported, not counted as a failure.
known=()
known_() { if printf '%s' "$3" | grep -Eq -- "$2"; then ok "$1"; else known+=("$1"); printf 'KNOWN %s (cu issue, same on stock Chromium)\n' "$1"; fi; }
# reject NAME ERE TEXT: TEXT must NOT match ERE.
reject() { if printf '%s' "$3" | grep -Eq -- "$2"; then bad "$1" "$3"; else ok "$1"; fi; }
# cu's session is the nearest ancestor process that is not a one-shot shell, and
# every \$(...) is a subshell of its own -- so a cu call inside \$(...) would be a
# new session (a new tab) each time. Calls therefore run in this shell and
# land in a variable: cux VAR ARGS...  (cu ARGS... for calls whose output is unused)
cu() { "$CU_SLIM" "$@" 2>>"$LOG"; }
cux() { local __v=$1 __o="$H/.out.$BASHPID"; shift; "$CU_SLIM" "$@" >"$__o" 2>>"$LOG"; printf -v "$__v" '%s' "$(cat "$__o")"; }
# ref_of SNAPSHOT_JSON 'role "name"' -> eN
ref_of() { printf '%s' "$1" | jq -r '.snapshot // empty' | grep -F -- "$2" | grep -o 'ref=e[0-9]*' | head -1 | cut -d= -f2; }
api() { # api METHOD PATH [JSON]: raw HTTP call to our daemon
  local tok; tok=$(jq -r .token "$H/server.json")
  curl -s -X "$1" -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
    ${3:+--data "$3"} "http://127.0.0.1:$HTTP_PORT$2"
}

before=$(user_cu_fingerprint)
echo "# user's cu before: $(echo $before)"
echo "# browser: $BROWSER (SLIM_PROFILE=${SLIM_PROFILE:-balanced})"
start_fixtures "$FIX_PORT" || exit 1
start_instance T "$H" "$HTTP_PORT" "$CDP_PORT" "$BROWSER" || exit 1
echo "# cu-slim daemon pid $INST_PID_T, browser pid $INST_BROWSER_T, data $H"

# --- daemon -------------------------------------------------------------------
cux R0 status; expect status '"running":true' "$R0"
cux R0 diagnostics; d=$R0
expect diagnostics.executable "$(basename "$BROWSER")" "$d"
expect diagnostics.product 'Chrome/1[0-9]{2}' "$d"
expect isolation.server.json "\"port\":$HTTP_PORT" "$(cat "$H/server.json")"
expect isolation.cdp "remote-debugging-port=$CDP_PORT" "$(tr '\0' ' ' </proc/$INST_BROWSER_T/cmdline)"

# --- navigate / snapshot / type / click / act ------------------------------------
cux R0 navigate "$BASE/form.html"; expect navigate '"settled":true' "$R0"
cux R0 snapshot; s=$R0
expect snapshot 'textbox \\"Full name\\"' "$s"
name=$(ref_of "$s" 'textbox "Full name"'); email=$(ref_of "$s" 'textbox "Email"')
color=$(ref_of "$s" 'combobox "Color'); agree=$(ref_of "$s" 'checkbox "I agree"')
notes=$(ref_of "$s" 'textbox "Notes"'); send=$(ref_of "$s" 'button "Send form"')
cux R0 type "$name" "Ada Lovelace"; expect type.ref '"ok":true' "$R0"
cux R0 act "[{\"do\":\"type\",\"ref\":\"$email\",\"text\":\"ada@example.com\"},{\"do\":\"select\",\"ref\":\"$color\",\"value\":\"green\"},{\"do\":\"click\",\"ref\":\"$agree\"},{\"do\":\"type\",\"ref\":\"$notes\",\"text\":\"hello slim\"},{\"do\":\"click\",\"ref\":\"$send\"}]"; r=$R0
expect act.batch '"ok":true' "$r"
expect act.snapshot '"snapshot"' "$r"
cux R0 text; expect text.after-act 'SENT name=Ada Lovelace email=ada@example.com color=green agree=true notes=hello slim' "$R0"
cux R0 act "[{\"do\":\"press\",\"key\":\"Tab\"}]"; expect act.press '"ok":true' "$R0"

# --- links, click by ref, popup, tabs --------------------------------------------
cu navigate "$BASE/links.html" >/dev/null
cux R0 snapshot; s=$R0
cux R0 click "$(ref_of "$s" 'link "Go to second page"')"; r=$R0
expect click.link '"ok":true' "$r"
cux R0 text; expect click.navigated 'Second page reached' "$R0"
cu navigate "$BASE/links.html" >/dev/null
cux R0 snapshot; s=$R0
cux R0 click "$(ref_of "$s" 'link "Open popup"')"; r=$R0
expect click.popup '"popup":true' "$r"
cux R0 tabs; t=$R0
expect tabs.list 'page2\.html' "$t"
pid=$(printf '%s' "$t" | jq -r '.. | objects | select((.url? // "") | test("page2.html")) | .id' 2>/dev/null | head -1)
cux R0 text --tab "$pid"; expect tabs.page-on-popup 'Second page reached' "$R0"
cux R0 tabs close "$pid"; expect tabs.close '"closed":true' "$R0"
cux R0 tabs; reject tabs.closed "$pid" "$R0"

# --- iframe ----------------------------------------------------------------------
cu navigate "$BASE/iframe.html" >/dev/null
cux R0 snapshot; s=$R0
expect iframe.snapshot 'frame: http' "$s"
cux R0 click "$(ref_of "$s" 'button "Frame button"')"; expect iframe.click '"ok":true' "$R0"
cux R0 text; expect iframe.text 'FRAME CLICKED' "$R0"
cux R0 snapshot; s=$R0
cu type "$(ref_of "$s" 'textbox "Frame input"')" "deep" >/dev/null
cux R0 text; expect iframe.type 'FRAME TYPED deep' "$R0"

# --- shadow DOM + inspect / mark / marks -------------------------------------------
cu navigate "$BASE/shadow.html" >/dev/null
cux R0 snapshot; s=$R0
sref=$(ref_of "$s" 'button "Shadow button"')
expect shadow.snapshot 'Shadow button' "$s"
cux R0 click "$sref"; expect shadow.click '"ok":true' "$R0"
cux R0 text; expect shadow.text 'SHADOW CLICKED' "$R0"
cux R0 inspect "$sref"; expect inspect.shadow-selector '>>>' "$R0"
cux R0 inspect --selector h1; expect inspect.selector 'h1' "$R0"
cux R0 mark "$sref" "button label too vague" --severity minor; expect mark '"ok":true|"id"|mark' "$R0"
cux R0 marks; expect marks 'button label too vague' "$R0"
cux R0 marks --md; expect marks.md 'minor' "$R0"
cu marks clear >/dev/null
cux R0 marks; reject marks.clear 'button label too vague' "$R0"

# --- long page, scroll, screenshots --------------------------------------------------
cu navigate "$BASE/long.html" >/dev/null
cux R0 snapshot; s=$R0
cux R0 act "[{\"do\":\"scroll\",\"to\":\"bottom\"},{\"do\":\"click\",\"ref\":\"$(ref_of "$s" 'button "Bottom button"')\"}]"; r=$R0
expect long.scroll+click '"ok":true' "$r"
# (cu text is capped in the page, so the bottom of a long page is read by eval)
cux R0 eval 'document.querySelector("button").textContent'; expect long.click 'BOTTOM CLICKED' "$R0"
cux R0 eval 'String(Math.round(scrollY) > 5000)'; expect long.scrolled '"value":"true"' "$R0"
cux R0 text; known_ long.text-capped-flag '"truncated":true' "$R0"   # cu caps text at 16000 chars but says truncated:false
png() { # png FILE: a real PNG of some size
  [ -s "$1" ] && [ "$(head -c 8 "$1" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a ] && [ "$(stat -c %s "$1")" -gt 2000 ]
}
cu shot "$H/full.png" >/dev/null; if png "$H/full.png"; then ok shot.png; else bad shot.png "$(ls -la "$H"/full.png 2>&1)"; fi
# (an element shot on this 400-paragraph page takes 3-6 s in cu on any Chromium and
# hits cu's 5 s DevTools timeout, so the element shot is taken on a short page)
cu navigate "$BASE/form.html" >/dev/null; cu snapshot >/dev/null
cu shot "$H/sel.png" --selector h1 --padding 8 >/dev/null; if png "$H/sel.png"; then ok shot.selector; else bad shot.selector "$(ls -la "$H"/sel.png 2>&1)"; fi
cu shot "$H/ann.png" --annotate all >/dev/null; if png "$H/ann.png"; then ok shot.annotate; else bad shot.annotate "$(ls -la "$H"/ann.png 2>&1)"; fi
cu shot "$H/s.jpg" >/dev/null; if [ "$(head -c 2 "$H/s.jpg" | od -An -tx1 | tr -d ' \n')" = ffd8 ]; then ok shot.jpeg; else bad shot.jpeg "$(ls -la "$H"/s.jpg 2>&1)"; fi

# --- eval / console -----------------------------------------------------------------
cux R0 eval 'document.title'; expect eval 'Form fixture' "$R0"
cu eval 'console.error("cuslim-console-marker"); "x"' >/dev/null
cux R0 console --level error; expect console 'cuslim-console-marker' "$R0"

# --- wait (act wait) ------------------------------------------------------------------
cu navigate "$BASE/late.html" >/dev/null
cux R0 text; reject wait.before 'LATE CONTENT ARRIVED' "$R0"
cux R0 act '[{"do":"wait","ms":1500}]'; expect wait.act '"ok":true' "$R0"
cux R0 text; expect wait.after 'LATE CONTENT ARRIVED' "$R0"

# --- login, cookies, secrets never echoed -----------------------------------------------
cu navigate "$BASE/login.html" >/dev/null
cux R0 snapshot; s=$R0
cu type "$(ref_of "$s" 'textbox "Username"')" ada >/dev/null
cux R0 type "$(ref_of "$s" 'textbox "Password"')" hunter2 --submit; r=$R0
reject login.no-secret-echo 'hunter2' "$r"
cu act '[{"do":"wait","ms":300}]' >/dev/null
cux R0 text; expect login.cookie 'Welcome, ada' "$R0"

# --- contexts: isolated cookie jars in one browser (Target.createBrowserContext) ----------
r=$(api POST "/v1/navigate?context=ctxA" "{\"url\":\"$BASE/account.html\"}")
expect context.open '"settled"' "$r"
expect context.isolated 'Not signed in' "$(api GET '/v1/text?context=ctxA')"
api POST "/v1/navigate?context=ctxA" "{\"url\":\"$BASE/login.html\"}" >/dev/null
s=$(api GET '/v1/snapshot?context=ctxA')
u=$(ref_of "$s" 'textbox "Username"'); p=$(ref_of "$s" 'textbox "Password"')
api POST '/v1/act?context=ctxA' "{\"actions\":[{\"do\":\"type\",\"ref\":\"$u\",\"text\":\"bob\"},{\"do\":\"type\",\"ref\":\"$p\",\"text\":\"hunter2\",\"submit\":true},{\"do\":\"wait\",\"ms\":300}]}" >/dev/null
expect context.own-login 'Welcome, bob' "$(api GET '/v1/text?context=ctxA')"
api POST "/v1/navigate?context=ctxB" "{\"url\":\"$BASE/account.html\"}" >/dev/null
expect context.second-isolated 'Not signed in' "$(api GET '/v1/text?context=ctxB')"
cu navigate "$BASE/account.html" >/dev/null
cux R0 text; expect context.default-untouched 'Welcome, ada' "$R0"
expect context.list '"ctxA".*"ctxB"|"ctxB".*"ctxA"' "$(api GET /v1/contexts)"
expect context.close '"closed":true' "$(api DELETE /v1/contexts/ctxA '{}')"
reject context.closed '"ctxA"' "$(api GET /v1/contexts)"
api DELETE /v1/contexts/ctxB '{}' >/dev/null

# --- session save / load ------------------------------------------------------------------
cux R0 session save slimtrip; expect session.save 'saved|"ok"|trip' "$R0"
cux R0 session load slimtrip; expect session.load 'load' "$R0"

# --- downloads / upload ----------------------------------------------------------------------
cu navigate "$BASE/download.html" >/dev/null
cux R0 snapshot; s=$R0
cux R0 click "$(ref_of "$s" 'link "Download report"')"; r=$R0
known_ download.click-reply '"download":"cuslim-report.txt"' "$r"   # cu listens for Page.downloadWillBegin; Chromium 156 no longer sends it to the page
for _ in $(seq 1 30); do [ -s "$H/downloads/cuslim-report.txt" ] && break; sleep 0.1; done
cux R0 downloads; expect downloads.list 'cuslim-report.txt' "$R0"
if cmp -s "$H/downloads/cuslim-report.txt" fixtures/report.txt; then ok downloads.file; else bad downloads.file "$(ls -la "$H/downloads" 2>&1)"; fi
cu navigate "$BASE/upload.html" >/dev/null
cux R0 snapshot; s=$R0
cux R0 upload "$(ref_of "$s" 'Attach file')" fixtures/report.txt; expect upload '"ok":true' "$R0"
cux R0 text; expect upload.page 'UPLOADED report.txt 25' "$R0"

# --- tabs: leased tab over the API, --tab, --new-tab ----------------------------------------
r=$(api POST /v1/tabs "{\"url\":\"$BASE/page2.html\",\"owner_pid\":\"$$\",\"fresh\":true}")
tid=$(printf '%s' "$r" | jq -r .id)
expect tab.open '"reused":false' "$r"
sleep 0.5
cux R0 text --tab "$tid"; expect tab.by-id 'Second page reached' "$R0"
cux R0 tabs close "$tid"; expect tab.close '"closed":true' "$R0"
cux R0 --new-tab navigate "$BASE/links.html"; r=$R0
expect new-tab.navigate '"settled":true' "$r"
grep -q 'working in a fresh tab' "$LOG" && ok new-tab.fresh || bad new-tab.fresh "$(tail -2 "$LOG")"
cux R0 targets; expect targets '\[|targets' "$R0"

# --- N parallel agents, one leased tab each (cu's parallel flow) ------------------------------
# Each flow is its own process (a subshell), so cu gives it its own tab:
# open (--new-tab) -> navigate -> snapshot -> act by ref -> judge text -> close.
cux R0 tabs; ntabs_before=$(printf '%s' "$R0" | jq '.tabs | length')
flow() {
  local i=$1 err=$H/flow.$i.err o=$H/flow.$i.out s n b tab
  "$CU_SLIM" --new-tab navigate "$BASE/form.html?flow=$i" >"$o" 2>"$err" || { echo "FAILNAV $i"; return; }
  tab=$(grep -o 'fresh tab ([0-9A-F]*)' "$err" | tr -dc '0-9A-F')
  "$CU_SLIM" snapshot >"$o" 2>>"$err"; s=$(cat "$o")
  n=$(ref_of "$s" 'textbox "Full name"'); b=$(ref_of "$s" 'button "Send form"')
  "$CU_SLIM" act "[{\"do\":\"type\",\"ref\":\"$n\",\"text\":\"flow $i\"},{\"do\":\"click\",\"ref\":\"$b\"}]" >"$o" 2>>"$err"
  "$CU_SLIM" text >"$o" 2>>"$err"
  if grep -q "SENT name=flow $i email" "$o"; then echo "PASS $i $tab"; else echo "FAILTEXT $i $tab"; fi
  [ -n "$tab" ] && "$CU_SLIM" tabs close "$tab" >/dev/null 2>>"$err"
}
t0=$(date +%s%N)
for i in $(seq 1 "$PARALLEL"); do ( flow "$i" >"$H/flow.$i.res" ) & done
wait_flows() { local i; for i in $(seq 1 "$PARALLEL"); do while ! [ -s "$H/flow.$i.res" ]; do sleep 0.1; done; done; }
wait_flows
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
npass=$(cat "$H"/flow.*.res | grep -c '^PASS')
tabs_seen=$(cat "$H"/flow.*.res | awk '{print $3}' | sort -u | grep -c .)
expect "parallel.$PARALLEL-flows ($npass/$PARALLEL in ${ms} ms)" "^$PARALLEL\$" "$npass"
expect "parallel.distinct-tabs ($tabs_seen)" "^$PARALLEL\$" "$tabs_seen"
sleep 0.3
cux R0 tabs; ntabs_after=$(printf '%s' "$R0" | jq '.tabs | length')
expect "parallel.tabs-closed ($ntabs_before -> $ntabs_after)" "^$ntabs_before\$" "$ntabs_after"
cux R0 status; expect status.after '"running":true' "$R0"
mem=$(tree_mem "$INST_BROWSER_T")
echo "# browser tree now: pss=$(( ${mem%% *} / 1024 )) MB, procs=${mem##* }"

# --- cu's own parallel bench on this browser (optional) ----------------------------------------
if [ -n "${CU_BENCH:-}" ] && [ -f "$HOME/cu-wt/slimium/bench/parallel_tabs.py" ]; then
  r=$(cd "$HOME/cu-wt/slimium" && CU_BROWSER=$BROWSER timeout 300 python3 bench/parallel_tabs.py --bin "$HOME/.local/bin/cu-slim-bin" --levels 5,20 --waves 1 2>&1 | tail -15)
  echo "$r" | sed 's/^/# bench: /'
  # Informational only: in cu 1.9.0 this bench sends "tab" in the JSON body,
  # which the daemon ignores, so its flows share one tab and most answer 502
  # on stock Chromium too (5: 3 ok/2 errors, 20: 6/14, both browsers alike).
  known_ cu.bench.parallel_tabs '"errors": 0' "$r"
fi

stop_instance T
after=$(user_cu_fingerprint)
if [ "$before" = "$after" ]; then ok user-cu-untouched; else bad user-cu-untouched "before: $before | after: $after"; fi
echo "# $pass passed, $fail failed${failed:+: ${failed[*]}}${known:+; known cu issues: ${known[*]}}"
[ "$fail" -eq 0 ]
