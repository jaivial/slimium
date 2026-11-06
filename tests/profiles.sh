#!/usr/bin/env bash
# Named user-profile registry, persistence, isolation, crash, and cu-wrapper tests.
# Uses only fresh temporary data and CDP 9450-9453; never reads or signals cu's browser.
set -u
cd "$(dirname "$0")/.."
BIN=${SLIM_BIN:-./slimium}
WRAP=${SLIM_WRAP:-./slimium-chrome}
[ -x "$BIN" ] && [ -x "$WRAP" ] || { echo "missing slimium binaries (run make)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required for the local fixture server"; exit 1; }

P0=${SLIM_PROFILE_TEST_PORT:-9450}
P1=$((P0 + 1))
P2=$((P0 + 2))
PW=$((P0 + 3))
HPORT=${SLIM_PROFILE_HTTP_PORT:-9469}
TMP=$(mktemp -d /tmp/slimium-profiles.XXXXXX)
ROOT="$TMP/profiles"
STATE="$TMP/state"
OUT="$TMP/out"
VICTIM="$TMP/outside-victim"
mkdir -p "$STATE" "$OUT" "$VICTIM"
printf 'must survive profile deletion tests
' >"$VICTIM/keep"
export SLIM_PROFILES_ROOT="$ROOT" SLIM_STATE_DIR="$STATE" SLIM_GOV=0
unset SLIM_USER_PROFILE SLIM_SESSION
A="profiles-a$$"
B="profiles-b$$"
DUP="profiles-dup$$"
HPID=
WPID=
: >"$OUT/pids"

cleanup() {
    "$BIN" --session "$A" stop >/dev/null 2>&1 || true
    "$BIN" --session "$B" stop >/dev/null 2>&1 || true
    "$BIN" --session "$DUP" stop >/dev/null 2>&1 || true
    if [ -n "$WPID" ]; then
        "$BIN" --port "$PW" cdp Browser.close >/dev/null 2>&1 || true
        # This is only the wrapper process launched and recorded below.
        kill "$WPID" 2>/dev/null || true
        wait "$WPID" 2>/dev/null || true
    fi
    if [ -n "$HPID" ]; then
        kill "$HPID" 2>/dev/null || true
        wait "$HPID" 2>/dev/null || true
    fi
    case "$TMP" in /tmp/slimium-profiles.*) rm -rf -- "$TMP" ;; esac
}
trap cleanup EXIT
trap 'exit 130' INT TERM

pass=0
fail=0
LAST=
RC=0
run_capture() { LAST=$("$@" 2>"$OUT/err"); RC=$?; return 0; }
ok() {
    local name=$1 observed=$2 expression=$3
    if eval "$expression"; then pass=$((pass + 1)); else
        echo "FAIL [$name] observed: $observed"; fail=$((fail + 1));
    fi
}
t_ok() {
    local name=$1; shift
    run_capture "$@"
    if [ "$RC" = 0 ]; then pass=$((pass + 1)); else
        echo "FAIL [$name] rc=$RC: $(head -c 300 "$OUT/err") $LAST"; fail=$((fail + 1));
    fi
}
t_re() {
    local name=$1 re=$2; shift 2
    run_capture "$@"
    if [ "$RC" = 0 ] && printf '%s' "$LAST" | grep -Eq -- "$re"; then pass=$((pass + 1)); else
        echo "FAIL [$name] rc=$RC re=/$re/: $(head -c 250 "$OUT/err") $(printf '%s' "$LAST" | head -c 300)";
        fail=$((fail + 1));
    fi
}
t_fail() {
    local name=$1 re=$2; shift 2
    run_capture "$@"
    if [ "$RC" != 0 ] && grep -Eq -- "$re" "$OUT/err"; then pass=$((pass + 1)); else
        echo "FAIL [$name] expected error /$re/, rc=$RC: $(head -c 300 "$OUT/err") $LAST"; fail=$((fail + 1));
    fi
}
must() { "$@" || { echo "FATAL: $*"; exit 1; }; }
state_all='"cookie":"cookie-marker-7451","local":"local-marker-7451","idb":"idb-marker-7451"'
state_none='"cookie":null,"local":null,"idb":null'
url_read="http://127.0.0.1:$HPORT/profile-read.html"
url_set="http://127.0.0.1:$HPORT/profile-set.html"
url_scroll="http://127.0.0.1:$HPORT/profile-scroll.html"

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
M_START=0
M_STOP=0
M_REOPEN=0
M_FIRST_REOPEN=0
M_RESTORE=0
start_named() {
    local ses=$1 port=$2 user=$3; shift 3
    local t0=$(now_ms) rc
    "$BIN" --session "$ses" start --user "$user" --port "$port" --no-gov "$@" >"$OUT/start" 2>"$OUT/err"
    rc=$?
    M_REOPEN=$(( $(now_ms) - t0 ))
    LAST=$(head -c 1000 "$OUT/start")
    [ "$rc" = 0 ] || { echo "start failed: $(head -c 400 "$OUT/err")"; return "$rc"; }
}
stop_session() {
    local ses=$1 t0=$(now_ms) rc
    "$BIN" --session "$ses" stop >"$OUT/stop" 2>"$OUT/err"
    rc=$?
    M_STOP=$(( $(now_ms) - t0 ))
    [ "$rc" = 0 ] || { echo "stop failed: $(head -c 400 "$OUT/err")"; return "$rc"; }
}
wait_wrapper() {
    local i
    for i in $(seq 1 100); do
        "$BIN" --port "$PW" status >/dev/null 2>&1 && return 0
        kill -0 "$WPID" 2>/dev/null || { echo "wrapper exited: $(head -c 300 "$OUT/wrapper-browser.log")"; return 1; }
        sleep 0.1
    done
    return 1
}
close_wrapper() {
    "$BIN" --port "$PW" cdp Browser.close >/dev/null 2>&1 || true
    local i
    for i in $(seq 1 60); do
        kill -0 "$WPID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$WPID" 2>/dev/null; then
        echo "FAIL [wrapper-close] recorded wrapper $WPID did not exit"
        fail=$((fail + 1))
        kill "$WPID" 2>/dev/null || true
    fi
    wait "$WPID" 2>/dev/null || true
    WPID=
}

# Registry basics and strict name validation.
t_re current-none '^none$' "$BIN" profile current
t_ok empty-list "$BIN" profile list
t_fail create-missing 'no user profile' "$BIN" --session "$A" start --user missing --port "$P0" --no-gov
t_re create-alpha 'created user profile alpha' "$BIN" profile create alpha --ua 'ProfileTests/1.0' --locale en-US
t_re create-beta 'created user profile beta' "$BIN" profile create beta
t_fail duplicate-name 'already exists' "$BIN" profile create alpha
for bad in '../escape' '.hidden' 'bad/name' 'bad name' ''; do
    t_fail "invalid-name-$bad" 'invalid user profile name' "$BIN" profile create "$bad"
done
t_re use-alpha 'default user profile: alpha' "$BIN" profile use alpha
t_re current-alpha '^alpha$' "$BIN" profile current
t_re list-current '"user":"alpha","current":true' "$BIN" --json profile list
t_re show-alpha 'ua +ProfileTests/1.0' "$BIN" profile show alpha
t_fail delete-needs-yes 'requires --yes' "$BIN" profile delete beta
ok root-mode "$(stat -c %a "$ROOT")" '[ "$(stat -c %a "$ROOT")" = 700 ]'
ok data-mode "$(stat -c %a "$ROOT/alpha/data")" '[ "$(stat -c %a "$ROOT/alpha/data")" = 700 ]'
ok metadata-mode "$(stat -c %a "$ROOT/alpha/profile.json")" '[ "$(stat -c %a "$ROOT/alpha/profile.json")" = 600 ]'

# Local multi-page fixture server, started and recorded by this test only.
(cd tests/fixtures && exec python3 -m http.server --bind 127.0.0.1 "$HPORT" >"$OUT/http.log" 2>&1) &
HPID=$!
printf '%s fixture-http
' "$HPID" >>"$OUT/pids"
sleep 0.3
kill -0 "$HPID" 2>/dev/null || { echo "fixture server failed"; exit 1; }

# 'profile use' supplies the default. No --user argument is needed for this start.
T0=$(now_ms)
run_capture "$BIN" --session "$A" start --port "$P0" --no-gov
M_START=$(( $(now_ms) - T0 ))
if [ "$RC" = 0 ]; then pass=$((pass + 1)); else echo "FATAL default start: $(head -c 400 "$OUT/err")"; exit 1; fi
t_re status-default-user '"user":"alpha"' "$BIN" --session "$A" --json status
t_re default-ua '^ProfileTests/1.0$' "$BIN" --session "$A" eval 'navigator.userAgent'
t_re default-locale '^en-US$' "$BIN" --session "$A" eval 'navigator.language'
t_ok fixture-write "$BIN" --session "$A" nav "$url_set"
t_re fixture-ready '^true$' "$BIN" --session "$A" eval 'window.profileFixtureReady'
sleep 1
must stop_session "$A"
pass=$((pass + 1))

# Explicit identity wins over a conflicting --user-data-dir. Login state needs no save.
must start_named "$A" "$P0" alpha --user-data-dir "$TMP/ignored-start-dir"
M_FIRST_REOPEN=$M_REOPEN
pass=$((pass + 1))
t_re named-data-dir "$ROOT/alpha/data" "$BIN" --session "$A" --json status
t_ok nav-read-after-graceful "$BIN" --session "$A" nav "$url_read"
t_re graceful-cookie-local-idb "$state_all" "$BIN" --session "$A" eval 'window.profileState'

# Save two pages, current tab, and scroll. Restore after a deliberate restart.
t_ok open-second-tab "$BIN" --session "$A" open "$url_scroll"
t_re scroll-second-tab 'scrolled to 0,[1-9][0-9]+' "$BIN" --session "$A" scroll-to '#bottom'
t_re save-two-tabs 'saved 2 pages for user profile alpha' "$BIN" profile save --session "$A"
run_capture "$BIN" --json profile show alpha
SAVED_CURRENT_OK=$(printf '%s' "$LAST" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("yes" if len(d["pages"]) == 2 and d["pages"][d["current_index"]]["url"] == d["current_url"] and d["current_url"].endswith("/profile-scroll.html") else "no")')
ok saved-current-consistency "$SAVED_CURRENT_OK" '[ "$SAVED_CURRENT_OK" = yes ]'
must stop_session "$A"
pass=$((pass + 1))
must start_named "$A" "$P0" alpha
pass=$((pass + 1))
T0=$(now_ms)
run_capture "$BIN" --session "$A" profile restore
M_RESTORE=$(( $(now_ms) - T0 ))
if [ "$RC" = 0 ]; then pass=$((pass + 1)); else echo "FAIL [restore] $(head -c 400 "$OUT/err") $LAST"; fail=$((fail + 1)); fi
run_capture "$BIN" --session "$A" --json tabs
TAB_COUNT=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["tabs"]))')
ok restore-two-tabs "$TAB_COUNT" '[ "$TAB_COUNT" = 2 ]'
CURRENT_URL=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(next(t["url"] for t in json.load(sys.stdin)["tabs"] if t["current"]))')
ok restore-current-tab "$CURRENT_URL" '[ "$CURRENT_URL" = "$url_scroll" ]'
run_capture "$BIN" --session "$A" tabs
ok restore-no-blank "$LAST" '! printf "%s" "$LAST" | grep -q about:blank'
t_re restored-storage "$state_all" "$BIN" --session "$A" eval 'window.profileState'
t_re restored-scroll '^\[0,[1-9][0-9]+\]$' "$BIN" --session "$A" eval '[scrollX,scrollY]'

# SIGKILL only the recorded browser's own process group, after storage settles.
sleep 3
run_capture "$BIN" --session "$A" --json status
BPID=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(json.load(sys.stdin)["pid"])' 2>/dev/null)
[ -n "$BPID" ] && [ -r "/proc/$BPID/cmdline" ] || { echo "FATAL no recorded test browser pid"; exit 1; }
printf '%s crash-browser
' "$BPID" >>"$OUT/pids"
CMD=$(tr '\0' ' ' <"/proc/$BPID/cmdline")
case "$CMD" in *"--remote-debugging-port=$P0"*"$ROOT/alpha/data"*|*"$ROOT/alpha/data"*"--remote-debugging-port=$P0"*) ;; *) echo "FATAL pid identity mismatch"; exit 1;; esac
PGID=$(ps -o pgid= -p "$BPID" | tr -d ' ')
[ "$PGID" = "$BPID" ] || { echo "FATAL test browser is not its own process group"; exit 1; }
if kill -KILL -- "-$BPID" 2>/dev/null; then pass=$((pass + 1)); else echo "FAIL [sigkill-own-browser]"; fail=$((fail + 1)); fi
for i in $(seq 1 60); do
    "$BIN" --port "$P0" status >/dev/null 2>&1 || break
    sleep 0.1
done
t_fail crash-port-down 'no browser|not running' "$BIN" --port "$P0" status
must start_named "$A" "$P0" alpha
pass=$((pass + 1))
t_ok nav-read-after-crash "$BIN" --session "$A" nav "$url_read"
t_re crash-cookie-local-idb "$state_all" "$BIN" --session "$A" eval 'window.profileState'

# Different profiles are isolated and may run together; duplicate use is refused.
must start_named "$B" "$P1" beta
pass=$((pass + 1))
t_ok nav-other-profile "$BIN" --session "$B" nav "$url_read"
t_re other-profile-no-state "$state_none" "$BIN" --session "$B" eval 'window.profileState'
t_fail duplicate-profile-use 'already in use by another browser' "$BIN" --session "$DUP" start --user alpha --port "$P2" --no-gov
t_re first-profile-still-alive "$state_all" "$BIN" --session "$A" eval 'window.profileState'
t_fail rename-active 'cannot rename active' "$BIN" profile rename alpha nope
t_fail delete-active 'cannot delete active' "$BIN" profile delete alpha --yes
must stop_session "$B"
pass=$((pass + 1))
must stop_session "$A"
pass=$((pass + 1))

# Daemon-like cu wrapper: env identity overrides cu's --user-data-dir on both starts.
SLIM_USER_PROFILE=alpha SLIM_WRAPPER_LOG="$OUT/wrapper.log" "$WRAP"     --headless=new --remote-debugging-port="$PW" --user-data-dir="$TMP/cu-ignored"     --no-first-run about:blank >"$OUT/wrapper-browser.log" 2>&1 &
WPID=$!
printf '%s wrapper-first
' "$WPID" >>"$OUT/pids"
must wait_wrapper
t_ok wrapper-nav-first "$BIN" --port "$PW" nav "$url_read"
t_re wrapper-state-first "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
ok wrapper-identity-arg "$(grep -c -- "--user-data-dir=$ROOT/alpha/data" "$OUT/wrapper.log")" 'grep -q -- "--user-data-dir=$ROOT/alpha/data" "$OUT/wrapper.log"'
ok wrapper-conflict-removed "$(grep -c -- '--user-data-dir=/.*cu-ignored' "$OUT/wrapper.log")" '! grep -q -- "--user-data-dir=.*cu-ignored" "$OUT/wrapper.log"'
close_wrapper
SLIM_USER_PROFILE=alpha "$WRAP" --headless=new --remote-debugging-port="$PW"     --user-data-dir="$TMP/cu-ignored-again" --no-first-run about:blank >"$OUT/wrapper-browser.log" 2>&1 &
WPID=$!
printf '%s wrapper-second
' "$WPID" >>"$OUT/pids"
must wait_wrapper
t_ok wrapper-nav-second "$BIN" --port "$PW" nav "$url_read"
t_re wrapper-state-after-daemon-restart "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
close_wrapper

# Registry mutation, no-secret output, no-follow deletion, and renamed data persistence.
t_ok rename-alpha "$BIN" profile rename alpha renamed
t_re default-follows-rename '^renamed$' "$BIN" profile current
t_re renamed-metadata '"user":"renamed"' "$BIN" --json profile show renamed
run_capture "$BIN" --json profile show renamed
if printf '%s' "$LAST" | grep -Eq 'cookie-marker-7451|local-marker-7451|idb-marker-7451'; then
    echo 'FAIL [no-secret-show] storage value appeared in profile show'; fail=$((fail + 1));
else pass=$((pass + 1)); fi
if grep -Eq 'cookie-marker-7451|local-marker-7451|idb-marker-7451' "$ROOT/renamed/profile.json"; then
    echo 'FAIL [no-secret-metadata] storage value appeared in metadata'; fail=$((fail + 1));
else pass=$((pass + 1)); fi
must start_named "$A" "$P0" renamed
pass=$((pass + 1))
t_ok nav-renamed-data "$BIN" --session "$A" nav "$url_read"
t_re renamed-data-survives "$state_all" "$BIN" --session "$A" eval 'window.profileState'
must stop_session "$A"
pass=$((pass + 1))
t_ok delete-beta "$BIN" profile delete beta --yes
t_fail deleted-beta-gone 'no user profile' "$BIN" profile show beta

t_ok create-symlink-case "$BIN" profile create linkcase
rmdir "$ROOT/linkcase/data"
ln -s "$VICTIM" "$ROOT/linkcase/data"
t_ok delete-symlink-case "$BIN" profile delete linkcase --yes
ok symlink-victim-survives "$([ -f "$VICTIM/keep" ])" '[ -f "$VICTIM/keep" ]'
t_fail relative-root 'must be an absolute path' env SLIM_PROFILES_ROOT=relative "$BIN" profile list
ln -s "$ROOT" "$TMP/root-link"
t_fail symlink-root 'not a real directory' env SLIM_PROFILES_ROOT="$TMP/root-link" "$BIN" profile list

# Measure a registry-only command's maximum RSS and the final stripped size.
if [ -x /usr/bin/time ]; then
    /usr/bin/time -f '%M' -o "$OUT/rss" "$BIN" profile list >/dev/null 2>"$OUT/err"
    PROFILE_RSS=$(tail -1 "$OUT/rss")
else PROFILE_RSS=unavailable; fi
BINARY_BYTES=$(stat -c %s "$BIN")
t_ok delete-renamed "$BIN" profile delete renamed --yes
t_re default-cleared-after-delete '^none$' "$BIN" profile current
ok outside-still-present "$([ -f "$VICTIM/keep" ])" '[ -f "$VICTIM/keep" ]'

echo "== profiles: $pass passed, $fail failed"
echo "== profile metrics: start_ms=$M_START stop_ms=$M_STOP reopen_start_ms=$M_FIRST_REOPEN restore_ms=$M_RESTORE registry_rss_kb=$PROFILE_RSS binary_bytes=$BINARY_BYTES"
[ "$fail" = 0 ]
