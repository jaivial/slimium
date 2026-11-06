#!/usr/bin/env bash
# Named user-profile registry, persistence, isolation, crash, cu-wrapper, and password-store
# pinning tests. Uses only fresh temporary data and CDP 9450-9453; never reads or signals cu's
# browser. Cookie databases are inspected only below the temporary SLIM_PROFILES_ROOT.
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
CU_PID=
CU_HPID=
CU_HOME="$TMP/cu-slim-home"
CU_HTTP=8792
CU_CDP=9392
CU_FIX=9467
CU_SLIM=${CU_SLIM:-$HOME/.local/bin/cu-slim}
: >"$OUT/pids"

cleanup() {
    trap - INT TERM
    cu_cleanup
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

# Password-store helpers. 'profile show' counts only encryption prefixes, never values.
show_field() { # user, python expression over the 'profile show' JSON object d
    "$BIN" --json profile show "$1" 2>>"$OUT/show.err" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$2" 2>/dev/null
}
cookie_counts() { show_field "$1" '"v10=%d v11=%d" % (d["cookies"]["v10"], d["cookies"]["v11"])'; }
sql_counts() { # Cross-check with SQLite itself (python3 stdlib, read-only immutable URI).
    python3 -c 'import sqlite3,sys
c=sqlite3.connect("file:%s?immutable=1" % sys.argv[1], uri=True)
r=[bytes(v[0])[:3] for v in c.execute("select encrypted_value from cookies")]
print("v10=%d v11=%d" % (r.count(b"v10"), r.count(b"v11")))' "$1" 2>/dev/null
}
craft_cookies() { # db path, comma list of fake encrypted_value prefixes (test-only fixture)
    python3 -c 'import sqlite3,sys
c=sqlite3.connect(sys.argv[1])
c.execute("CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR)")
c.execute("CREATE TABLE cookies(creation_utc INTEGER NOT NULL,host_key TEXT NOT NULL,top_frame_site_key TEXT NOT NULL,name TEXT NOT NULL,value TEXT NOT NULL,encrypted_value BLOB NOT NULL,path TEXT NOT NULL,expires_utc INTEGER NOT NULL,is_secure INTEGER NOT NULL,is_httponly INTEGER NOT NULL,last_access_utc INTEGER NOT NULL,has_expires INTEGER NOT NULL,is_persistent INTEGER NOT NULL,priority INTEGER NOT NULL,samesite INTEGER NOT NULL,source_scheme INTEGER NOT NULL,source_port INTEGER NOT NULL,last_update_utc INTEGER NOT NULL,source_type INTEGER NOT NULL,has_cross_site_ancestor INTEGER NOT NULL)")
for i,p in enumerate(x for x in sys.argv[2].split(",") if x):
    c.execute("INSERT INTO cookies VALUES(?,?,?,?,?,?,?,0,0,0,0,0,1,1,0,0,0,0,0,0)", (i,"127.0.0.1","","fixture%d"%i,"",p.encode()+bytes(32),"/"))
c.commit()' "$1" "$2"
}
legacy_profile() { # name, cookie prefixes ("" = no Cookies DB): registry entry without password_store
    mkdir -p -m 700 "$ROOT/$1" "$ROOT/$1/data" "$ROOT/$1/data/Default"
    (umask 077; printf '{"version":1,"name":"%s","created_ms":1,"updated_ms":1}\n' "$1" >"$ROOT/$1/profile.json")
    [ -z "$2" ] || craft_cookies "$ROOT/$1/data/Default/Cookies" "$2"
}
strip_store() { python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d.pop("password_store",None); open(p,"w").write(json.dumps(d))' "$ROOT/$1/profile.json"; }
set_store_raw() { python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["password_store"]=sys.argv[2]; open(p,"w").write(json.dumps(d))' "$ROOT/$1/profile.json" "$2"; }
cmdline_of_session() { # session -> its browser command line
    local pid
    pid=$("$BIN" --session "$1" --json status 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["pid"])' 2>/dev/null)
    [ -n "$pid" ] && tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null
}
wrap_launch() { # user, caller args...: a cu-style daemon launch through slimium-chrome
    local user=$1; shift
    : >"$OUT/wrap-ps.log"
    SLIM_USER_PROFILE=$user SLIM_WRAPPER_LOG="$OUT/wrap-ps.log" "$WRAP" --headless=new --remote-debugging-port="$PW" \
        --user-data-dir="$TMP/cu-ps-ignored" --no-first-run "$@" about:blank >"$OUT/wrapper-browser.log" 2>&1 &
    WPID=$!
    printf '%s wrapper-%s\n' "$WPID" "$user" >>"$OUT/pids"
    wait_wrapper
}
set_fixture_state() { # session: write cookies + localStorage + IndexedDB, wait until stored
    local i
    "$BIN" --session "$1" nav "$url_set" >/dev/null 2>&1 || return 1
    for i in $(seq 1 50); do
        if [ "$("$BIN" --session "$1" eval 'window.profileFixtureReady' 2>/dev/null)" = true ]; then
            # Leave the read page open: the wrapper keeps Chromium's restored tabs, and stateless
            # --port commands may pick either tab, so every restored tab must be a read page.
            sleep 1; "$BIN" --session "$1" nav "$url_read" >/dev/null 2>&1; return $?
        fi
        sleep 0.1
    done
    return 1
}

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
M_START=0
M_STOP=0
M_REOPEN=0
M_FIRST_REOPEN=0
M_RESTORE=0
M_CU_STOP=0
M_CU_RESTART=0
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


tcp_wait() { # port, recorded process id: never invokes a cu CLI auto-start path
    local port=$1 pid=$2 i
    for i in $(seq 1 200); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
cu_cli() {
    # Refuse a CLI call after our daemon dies: it might auto-start an unrecorded one.
    kill -0 "$CU_PID" 2>/dev/null || return 1
    CU_SLIM_HOME="$CU_HOME" CU_SLIM_PORT="$CU_HTTP" CU_SLIM_CDP_PORT="$CU_CDP" "$CU_SLIM" "$@"
}
cu_start_daemon() {
    CU_FORCE_KILL=0
    mkdir -p "$CU_HOME"
    CU_SLIM_HOME="$CU_HOME" CU_SLIM_PORT="$CU_HTTP" CU_SLIM_CDP_PORT="$CU_CDP"         CU_SLIM_BROWSER="$WRAP" SLIM_USER_PROFILE=cuuser SLIM_GOV=0         setsid "$CU_SLIM" daemon >"$OUT/cu-daemon.log" 2>&1 </dev/null &
    CU_PID=$!
    printf '%s cu-slim-daemon\n' "$CU_PID" >>"$OUT/pids"
    tcp_wait "$CU_HTTP" "$CU_PID" && tcp_wait "$CU_CDP" "$CU_PID" || return 1
    cu_cli diagnostics >"$OUT/cu-diagnostics.json" 2>"$OUT/cu-diagnostics.err"
}
cu_stop_daemon() {
    [ -n "$CU_PID" ] || return 0
    # Browser.close flushes Chromium state; SIGTERM then closes the recorded daemon.
    "$BIN" --port "$CU_CDP" cdp Browser.close >"$OUT/cu-browser-close.out" 2>"$OUT/cu-browser-close.err" || true
    kill -TERM "$CU_PID" 2>/dev/null || true
    local i
    for i in $(seq 1 80); do
        kill -0 "$CU_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$CU_PID" 2>/dev/null; then
        kill -KILL "$CU_PID" 2>/dev/null || true
        CU_FORCE_KILL=1
    else CU_FORCE_KILL=0; fi
    wait "$CU_PID" 2>/dev/null || true
    CU_PID=
}
cu_cleanup() {
    cu_stop_daemon
    if [ -n "$CU_HPID" ]; then
        kill "$CU_HPID" 2>/dev/null || true
        wait "$CU_HPID" 2>/dev/null || true
        CU_HPID=
    fi
}
cu_navigate() {
    local name=$1 rc
    cu_cli navigate "$CU_URL" >"$OUT/cu-navigate.json" 2>"$OUT/cu-navigate.err"
    rc=$?
    CU_NAV_OK=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("yes" if d.get("settled") is True else "no")' "$OUT/cu-navigate.json" 2>/dev/null)
    ok "$name" "$CU_NAV_OK" '[ "$rc" = 0 ] && [ "$CU_NAV_OK" = yes ]'
}
cu_assert_state() {
    local name=$1 expected=$2 rc
    cu_cli eval 'JSON.stringify({session:document.cookie.includes("cu_session="),persistent:document.cookie.includes("cu_persistent="),local:localStorage.getItem("cu_local")==="present"})' >"$OUT/cu-eval.json" 2>"$OUT/cu-eval.err"
    rc=$?
    CU_STATE_OK=$(python3 -c 'import json,sys
def find(x):
    if isinstance(x,dict):
        if "value" in x: return x["value"]
        for y in x.values():
            z=find(y)
            if z is not None: return z
    elif isinstance(x,list):
        for y in x:
            z=find(y)
            if z is not None: return z
    return None
v=find(json.load(open(sys.argv[1]))); v=json.loads(v) if isinstance(v,str) else v
got=all(v[k] for k in ("session","persistent","local")); print("yes" if got else "no")' "$OUT/cu-eval.json" 2>/dev/null)
    ok "$name" "rc=$rc parsed=$CU_STATE_OK" '[ "$rc" = 0 ] && [ "$CU_STATE_OK" = "$expected" ]'
}

trap cleanup EXIT
trap 'exit 130' INT TERM

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
t_re show-alpha-store 'password_store basic' "$BIN" profile show alpha
ok metadata-store-default "$(cat "$ROOT/alpha/profile.json")" 'grep -q "\"password_store\":\"basic\"" "$ROOT/alpha/profile.json"'
t_fail create-bad-store 'bad password store' "$BIN" profile create badstore --password-store gnome
t_fail bad-store-not-created 'no user profile' "$BIN" profile show badstore
t_fail delete-needs-yes 'requires --yes' "$BIN" profile delete beta
ok root-mode "$(stat -c %a "$ROOT")" '[ "$(stat -c %a "$ROOT")" = 700 ]'
ok data-mode "$(stat -c %a "$ROOT/alpha/data")" '[ "$(stat -c %a "$ROOT/alpha/data")" = 700 ]'
ok metadata-mode "$(stat -c %a "$ROOT/alpha/profile.json")" '[ "$(stat -c %a "$ROOT/alpha/profile.json")" = 600 ]'
ok preferences-mode "$(stat -c %a "$ROOT/alpha/data/Default/Preferences")" '[ "$(stat -c %a "$ROOT/alpha/data/Default/Preferences")" = 600 ]'
# Simulate a user setting plus another Chromium preference; start must preserve both.
python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["session"]["restore_on_startup"]=0; d["session"]["profile_test_nested"]="kept"; d["profile_test_keep"]={"value":42}; open(p,"w").write(json.dumps(d))' "$ROOT/alpha/data/Default/Preferences"

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
DEFAULT_CMD=$(cmdline_of_session "$A")
ok default-start-pins-basic "$DEFAULT_CMD" 'printf "%s" "$DEFAULT_CMD" | grep -q -- "--password-store=basic"'
PREF_OK=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("yes" if d["session"]["restore_on_startup"] == 1 and d["session"]["profile_test_nested"] == "kept" and d["profile_test_keep"]["value"] == 42 else "no")' "$ROOT/alpha/data/Default/Preferences")
ok preferences-preserved "$PREF_OK" '[ "$PREF_OK" = yes ]'
ok preferences-mode-after-patch "$(stat -c %a "$ROOT/alpha/data/Default/Preferences")" '[ "$(stat -c %a "$ROOT/alpha/data/Default/Preferences")" = 600 ]'
t_re default-ua '^ProfileTests/1.0$' "$BIN" --session "$A" eval 'navigator.userAgent'
t_re default-locale '^en-US$' "$BIN" --session "$A" eval 'navigator.language'
t_ok fixture-write "$BIN" --session "$A" nav "$url_set"
FIXTURE_READY=0
for i in $(seq 1 50); do
    run_capture "$BIN" --session "$A" eval 'window.profileFixtureReady'
    if [ "$RC" = 0 ] && [ "$LAST" = true ]; then FIXTURE_READY=1; break; fi
    sleep 0.1
done
ok fixture-ready "$LAST" '[ "$FIXTURE_READY" = 1 ]'
sleep 1
must stop_session "$A"
pass=$((pass + 1))

# Explicit identity wins over a conflicting --user-data-dir. Login state needs no save.
must start_named "$A" "$P0" alpha --user-data-dir "$TMP/ignored-start-dir"
M_FIRST_REOPEN=$M_REOPEN
pass=$((pass + 1))
run_capture "$BIN" --session "$A" --json tabs
PRUNED_TABS=$(printf '%s' "$LAST" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("yes" if len(d["tabs"]) == 1 and d["tabs"][0]["url"] == "about:blank" else "no")')
ok named-start-prunes-automatic-tabs "$PRUNED_TABS" '[ "$PRUNED_TABS" = yes ]'
t_re named-data-dir "$ROOT/alpha/data" "$BIN" --session "$A" --json status
t_ok nav-read-after-graceful "$BIN" --session "$A" nav "$url_read"
t_re graceful-cookie-local-idb "$state_all" "$BIN" --session "$A" eval 'window.profileState'
t_re graceful-session-cookie '"session":"session-marker-7451"' "$BIN" --session "$A" eval 'window.profileState'

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
if [ "$RC" = 0 ] && printf '%s' "$LAST" | grep -Eq '^restored 2 pages for user profile alpha \(current [01]\)$'; then
    pass=$((pass + 1));
else echo "FAIL [restore] $(head -c 400 "$OUT/err") $LAST"; fail=$((fail + 1)); fi
run_capture "$BIN" --session "$A" --json tabs
TAB_COUNT=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["tabs"]))')
ok restore-two-tabs "$TAB_COUNT" '[ "$TAB_COUNT" = 2 ]'
CURRENT_URL=$(printf '%s' "$LAST" | python3 -c 'import json,sys; print(next(t["url"] for t in json.load(sys.stdin)["tabs"] if t["current"]))')
ok restore-current-tab "$CURRENT_URL" '[ "$CURRENT_URL" = "$url_scroll" ]'
run_capture "$BIN" --session "$A" tabs
ok restore-no-blank "$LAST" '! printf "%s" "$LAST" | grep -q about:blank'
t_re restored-storage "$state_all" "$BIN" --session "$A" eval 'window.profileState'
t_re restored-session-cookie '"session":"session-marker-7451"' "$BIN" --session "$A" eval 'window.profileState'
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
t_re other-profile-no-session '"session":null' "$BIN" --session "$B" eval 'window.profileState'
t_fail duplicate-profile-use 'already in use by another browser' "$BIN" --session "$DUP" start --user alpha --port "$P2" --no-gov
t_re first-profile-still-alive "$state_all" "$BIN" --session "$A" eval 'window.profileState'
t_fail rename-active 'cannot rename active' "$BIN" profile rename alpha nope
t_fail delete-active 'cannot delete active' "$BIN" profile delete alpha --yes
must stop_session "$B"
pass=$((pass + 1))
must stop_session "$A"
pass=$((pass + 1))

# An explicit Chromium session restore request opts out of automatic tab pruning.
t_ok create-explicit-restore "$BIN" profile create explicit
must start_named "$DUP" "$P2" explicit
pass=$((pass + 1))
t_ok explicit-restore-write "$BIN" --session "$DUP" nav "$url_read"
must stop_session "$DUP"
pass=$((pass + 1))
must start_named "$DUP" "$P2" explicit -- --restore-last-session
pass=$((pass + 1))
run_capture "$BIN" --session "$DUP" --json tabs
EXPLICIT_KEPT=$(printf '%s' "$LAST" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("yes" if any(t["url"].endswith("/profile-read.html") for t in d["tabs"]) else "no")')
ok explicit-restore-tabs-kept "$EXPLICIT_KEPT" '[ "$EXPLICIT_KEPT" = yes ]'
must stop_session "$DUP"
pass=$((pass + 1))
t_ok delete-explicit-restore "$BIN" profile delete explicit --yes

# Daemon-like cu wrapper: env identity overrides cu's --user-data-dir on both starts.
SLIM_USER_PROFILE=alpha SLIM_WRAPPER_LOG="$OUT/wrapper.log" "$WRAP"     --headless=new --remote-debugging-port="$PW" --user-data-dir="$TMP/cu-ignored"     --no-first-run about:blank >"$OUT/wrapper-browser.log" 2>&1 &
WPID=$!
printf '%s wrapper-first
' "$WPID" >>"$OUT/pids"
must wait_wrapper
t_ok wrapper-nav-first "$BIN" --port "$PW" nav "$url_read"
t_re wrapper-state-first "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
t_re wrapper-session-first '"session":"session-marker-7451"' "$BIN" --port "$PW" eval 'window.profileState'
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
t_re wrapper-session-after-restart '"session":"session-marker-7451"' "$BIN" --port "$PW" eval 'window.profileState'
close_wrapper

# ---- Password store pinning (the cu data-loss bug) ----
# A Chromium launched with another --password-store than the one that encrypted a cookie cannot
# decrypt it and deletes it. Named profiles pin the store; start and the wrapper enforce it.
t_re ps-create 'password store basic' "$BIN" profile create psbasic
must start_named "$B" "$P1" psbasic -- --password-store=gnome-libsecret
pass=$((pass + 1))
ok ps-start-caller-replaced "$(cat "$OUT/err")" 'grep -q "pins --password-store=basic; replaced caller --password-store=gnome-libsecret" "$OUT/err"'
ok ps-start-json-store "$LAST" 'printf "%s" "$LAST" | grep -q "user psbasic"'
PS_CMD=$(cmdline_of_session "$B")
ok ps-start-cmdline-basic "$PS_CMD" 'printf "%s" "$PS_CMD" | grep -q -- "--password-store=basic" && ! printf "%s" "$PS_CMD" | grep -q -- "gnome-libsecret"'
must set_fixture_state "$B"
pass=$((pass + 1))
must stop_session "$B"
pass=$((pass + 1))
PS_DB="$ROOT/psbasic/data/Default/Cookies"
PS_COUNTS=$(cookie_counts psbasic)
ok ps-written-v10 "$PS_COUNTS" '[ "$PS_COUNTS" = "v10=2 v11=0" ]'
PS_SQL=$(sql_counts "$PS_DB")
ok ps-scanner-matches-sqlite "ours=$PS_COUNTS sqlite=$PS_SQL" '[ "$PS_SQL" = "$PS_COUNTS" ]'
# cu-style relaunches: caller basic, caller gnome-libsecret (cu's opposite), no caller value.
for CALLER in --password-store=basic --password-store=gnome-libsecret none; do
    if [ "$CALLER" = none ]; then must wrap_launch psbasic; else must wrap_launch psbasic "$CALLER"; fi
    t_ok "ps-wrap-nav[$CALLER]" "$BIN" --port "$PW" nav "$url_read"
    t_re "ps-wrap-cookies[$CALLER]" "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
    t_re "ps-wrap-session[$CALLER]" '"session":"session-marker-7451"' "$BIN" --port "$PW" eval 'window.profileState'
    WLOG=$(cat "$OUT/wrap-ps.log")
    ok "ps-wrap-argv[$CALLER]" "$WLOG" '[ "$(grep -o -- "password-store=[a-z0-9-]*" "$OUT/wrap-ps.log")" = "password-store=basic" ]'
    if [ "$CALLER" = --password-store=gnome-libsecret ]; then
        ok ps-wrap-override-logged "$(head -c 300 "$OUT/wrapper-browser.log")" 'grep -q "slimium-chrome: user profile .psbasic. pins --password-store=basic; replaced caller --password-store=gnome-libsecret" "$OUT/wrapper-browser.log"'
    else
        ok "ps-wrap-no-log[$CALLER]" "$(head -c 300 "$OUT/wrapper-browser.log")" '! grep -q "replaced caller" "$OUT/wrapper-browser.log"'
    fi
    close_wrapper
    PS_AFTER=$(cookie_counts psbasic)
    ok "ps-wrap-db-intact[$CALLER]" "$PS_AFTER" '[ "$PS_AFTER" = "v10=2 v11=0" ]'
done

# argv rewriting without a real engine: a fake engine records argv and that it ran.
FAKE="$TMP/fake-chrome"
printf '#!/bin/sh\ntouch "%s/fake-ran"\nexit 0\n' "$TMP" >"$FAKE"
chmod 700 "$FAKE"
SLIM_CHROMIUM="$FAKE" SLIM_USER_PROFILE=psbasic SLIM_WRAPPER_LOG="$OUT/fake.log" "$WRAP" --password-store gnome-libsecret \
    -password-store=kwallet5 --password-store=basic --remote-debugging-port="$PW" about:blank 2>"$OUT/fake.err"
FAKE_LOG=$(cat "$OUT/fake.log")
ok ps-fake-one-store "$FAKE_LOG" '[ "$(grep -o -- "password-store[^ ]*" "$OUT/fake.log")" = "password-store=basic" ] && ! grep -q gnome-libsecret "$OUT/fake.log" && grep -q "about:blank" "$OUT/fake.log"'
ok ps-fake-logs-each "$(cat "$OUT/fake.err")" '[ "$(grep -c "replaced caller" "$OUT/fake.err")" = 2 ]'
: >"$OUT/fake.log"
SLIM_CHROMIUM="$FAKE" SLIM_WRAPPER_LOG="$OUT/fake.log" "$WRAP" --password-store=gnome-libsecret --remote-debugging-port="$PW" about:blank 2>"$OUT/fake.err"
FAKE_LOG=$(cat "$OUT/fake.log")
ok ps-unnamed-unchanged "$FAKE_LOG" '[ "$(grep -o -- "password-store=[a-z0-9-]*" "$OUT/fake.log")" = "password-store=gnome-libsecret" ] && ! grep -q "user-data-dir" "$OUT/fake.log" && [ ! -s "$OUT/fake.err" ]'

# Guard: a tampered pin (basic) over keyring-encrypted (v11) cookies refuses to launch.
t_ok ps-create-guard "$BIN" profile create psguard
craft_cookies "$ROOT/psguard/data/Default/Cookies" v10,v11,v11
GUARD_SUM=$(sha256sum "$ROOT/psguard/data/Default/Cookies")
t_fail ps-guard-start 'pins --password-store=basic but 2 of its cookies are encrypted with an OS keyring.*refusing to start' \
    "$BIN" --session "$DUP" start --user psguard --port "$P2" --no-gov
t_fail ps-guard-port-unused 'not running|no browser' "$BIN" --port "$P2" status
rm -f "$TMP/fake-ran"
SLIM_CHROMIUM="$FAKE" SLIM_USER_PROFILE=psguard "$WRAP" --password-store=basic about:blank 2>"$OUT/fake.err"
GUARD_RC=$?
ok ps-guard-wrapper "rc=$GUARD_RC $(cat "$OUT/fake.err")" '[ "$GUARD_RC" = 127 ] && grep -q "refusing to start" "$OUT/fake.err" && [ ! -e "$TMP/fake-ran" ]'
ok ps-guard-db-untouched "$(sha256sum "$ROOT/psguard/data/Default/Cookies")" '[ "$(sha256sum "$ROOT/psguard/data/Default/Cookies")" = "$GUARD_SUM" ]'
t_fail ps-repin-basic-refused 'refusing to pin .basic.' "$BIN" profile password-store psguard basic
t_re ps-repin-keyring 'now pins password store gnome-libsecret' "$BIN" profile password-store psguard gnome-libsecret
t_fail ps-repin-other-keyring 'refusing to pin .kwallet5.' "$BIN" profile password-store psguard kwallet5
t_re ps-guard-show '"password_store":"gnome-libsecret","cookies":\{"v10":1,"v11":2,"other":0\}' "$BIN" --json profile show psguard
# A keyring pin with v11 cookies needs a session bus; without one Chromium could not decrypt them.
t_fail ps-guard-no-bus 'no session D-Bus' env -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$TMP/no-runtime" \
    SLIM_CHROMIUM="$FAKE" "$BIN" --session "$DUP" start --user psguard --port "$P2" --no-gov
t_ok ps-delete-guard "$BIN" profile delete psguard --yes

# Legacy profiles (no password_store): detected from the cookie database, persisted atomically.
legacy_profile legv10 v10,v10
legacy_profile legv11 v10,v11
legacy_profile legkde v11
legacy_profile legnone ""
legacy_profile legbad ""
printf 'SQLite format 3\0garbage' >"$ROOT/legbad/data/Default/Cookies"
run_capture "$BIN" --json profile show legv10
ok ps-legacy-v10 "$LAST $(cat "$OUT/err")" 'printf "%s" "$LAST" | grep -q "\"password_store\":\"basic\"" && grep -q "pinned password store basic (detected: 2 v10, 0 v11" "$OUT/err"'
run_capture env XDG_CURRENT_DESKTOP=GNOME "$BIN" --json profile show legv11
ok ps-legacy-v11 "$LAST" 'printf "%s" "$LAST" | grep -q "\"password_store\":\"gnome-libsecret\""'
run_capture env XDG_CURRENT_DESKTOP=KDE KDE_SESSION_VERSION=6 "$BIN" --json profile show legkde
ok ps-legacy-kde "$LAST" 'printf "%s" "$LAST" | grep -q "\"password_store\":\"kwallet6\""'
run_capture "$BIN" --json profile show legnone
ok ps-legacy-none "$LAST" 'printf "%s" "$LAST" | grep -q "\"password_store\":\"basic\""'
t_fail ps-legacy-corrupt 'cannot detect the password store' "$BIN" profile show legbad
ok ps-legacy-corrupt-not-pinned "$(cat "$ROOT/legbad/profile.json")" '! grep -q password_store "$ROOT/legbad/profile.json"'
for L in legv10 legv11 legkde legnone; do
    ok "ps-legacy-persisted[$L]" "$(cat "$ROOT/$L/profile.json")" 'grep -q "\"password_store\":" "$ROOT/$L/profile.json" && [ "$(stat -c %a "$ROOT/$L/profile.json")" = 600 ]'
done
ok ps-legacy-no-tmp "$(ls "$ROOT"/leg*/)" '! ls "$ROOT"/leg*/ | grep -q "\.tmp\."'
# Second use reads the pin (even from another desktop) instead of re-detecting.
run_capture env XDG_CURRENT_DESKTOP=KDE "$BIN" --json profile show legv11
ok ps-legacy-stable "$LAST $(cat "$OUT/err")" 'printf "%s" "$LAST" | grep -q "\"password_store\":\"gnome-libsecret\"" && ! grep -q detected "$OUT/err"'
# A pinned profile with an unreadable database still opens (warning) and shows 'unknown'.
set_store_raw legbad basic
t_re ps-pinned-corrupt-show 'cookies +unknown' "$BIN" profile show legbad
for L in legv10 legv11 legkde legnone legbad; do t_ok "ps-legacy-delete[$L]" "$BIN" profile delete "$L" --yes; done
# The user's scenario on a real Chromium profile: strip the pin, then a cu-style launch with the
# opposite caller store must detect basic from the v10 cookies and keep them.
strip_store psbasic
must wrap_launch psbasic --password-store=gnome-libsecret
t_ok ps-legacy-wrap-nav "$BIN" --port "$PW" nav "$url_read"
t_re ps-legacy-wrap-cookies "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
ok ps-legacy-wrap-detected "$(head -c 400 "$OUT/wrapper-browser.log")" 'grep -q "pinned password store basic (detected: 2 v10, 0 v11" "$OUT/wrapper-browser.log"'
close_wrapper
ok ps-legacy-wrap-persisted "$(cat "$ROOT/psbasic/profile.json")" 'grep -q "\"password_store\":\"basic\"" "$ROOT/psbasic/profile.json"'

# A keyring-pinned profile survives cu's forced basic (only with a usable keyring here).
t_re ps-create-ring 'password store gnome-libsecret' "$BIN" profile create psring --password-store gnome-libsecret
must start_named "$B" "$P1" psring
pass=$((pass + 1))
must set_fixture_state "$B"
pass=$((pass + 1))
must stop_session "$B"
pass=$((pass + 1))
RING_COUNTS=$(cookie_counts psring)
case "$RING_COUNTS" in
    "v10=0 v11=0"|*" v11=0")
        echo "== keyring checks skipped: no usable keyring here (gnome-libsecret launch wrote $RING_COUNTS)" ;;
    *)
        ok ps-ring-v11 "$RING_COUNTS" '[ "$RING_COUNTS" = "v10=0 v11=2" ]'
        for ROUND in pinned legacy; do
            # 'legacy': the exact field failure (profile created without the pin, cu passes basic).
            [ "$ROUND" = legacy ] && strip_store psring
            must wrap_launch psring --password-store=basic
            t_ok "ps-ring-nav[$ROUND]" "$BIN" --port "$PW" nav "$url_read"
            t_re "ps-ring-cookies[$ROUND]" "$state_all" "$BIN" --port "$PW" eval 'window.profileState'
            t_re "ps-ring-session[$ROUND]" '"session":"session-marker-7451"' "$BIN" --port "$PW" eval 'window.profileState'
            ok "ps-ring-replaced[$ROUND]" "$(head -c 300 "$OUT/wrapper-browser.log")" 'grep -q "pins --password-store=gnome-libsecret; replaced caller --password-store=basic" "$OUT/wrapper-browser.log"'
            close_wrapper
            RING_AFTER=$(cookie_counts psring)
            ok "ps-ring-db-intact[$ROUND]" "$RING_AFTER" '[ "$RING_AFTER" = "v10=0 v11=2" ]'
        done
        set_store_raw psring basic
        RING_SUM=$(sha256sum "$ROOT/psring/data/Default/Cookies")
        t_fail ps-ring-tampered-start 'refusing to start' "$BIN" --session "$B" start --user psring --port "$P1" --no-gov
        ok ps-ring-tampered-untouched "$RING_SUM" '[ "$(sha256sum "$ROOT/psring/data/Default/Cookies")" = "$RING_SUM" ]'
        t_ok ps-ring-repin "$BIN" profile password-store psring gnome-libsecret
        must start_named "$B" "$P1" psring
        pass=$((pass + 1))
        t_ok ps-ring-final-nav "$BIN" --session "$B" nav "$url_read"
        t_re ps-ring-final-cookies "$state_all" "$BIN" --session "$B" eval 'window.profileState'
        must stop_session "$B"
        pass=$((pass + 1)) ;;
esac
t_ok ps-delete-ring "$BIN" profile delete psring --yes
t_ok ps-delete-basic "$BIN" profile delete psbasic --yes


# Real cu 1.9.0 daemon restart, explicitly managed (cu-slim has no stop/start lifecycle).
if [ "${SLIM_PROFILE_SKIP_CU:-0}" = 1 ]; then
    echo '== cu-slim daemon restart checks skipped by SLIM_PROFILE_SKIP_CU=1'
elif [ ! -x "$CU_SLIM" ]; then
    echo "== cu-slim daemon restart checks skipped: $CU_SLIM is unavailable (optional integration dependency)"
elif ss -ltnH | awk '{print $4}' | grep -Eq ":($CU_HTTP|$CU_CDP|$CU_FIX)$"; then
    echo 'FAIL [cu-ports-free] 8792, 9392, or 9467 is already occupied; refusing to touch it'
    fail=$((fail + 1))
else
    t_ok create-cu-profile "$BIN" profile create cuuser
    CU_URL="http://127.0.0.1:$CU_FIX/profile-read.html"
    (cd tests/fixtures && exec python3 -m http.server --bind 127.0.0.1 "$CU_FIX" >"$OUT/cu-http.log" 2>&1) &
    CU_HPID=$!
    printf '%s cu-fixture-http\n' "$CU_HPID" >>"$OUT/pids"
    tcp_wait "$CU_FIX" "$CU_HPID" || { echo 'FATAL cu fixture server did not start'; exit 1; }
    must cu_start_daemon
    cu_navigate cu-daemon-first-navigate
    cu_cli eval 'document.cookie="cu_session=present;path=/";document.cookie="cu_persistent=present;Max-Age=3600;path=/";localStorage.setItem("cu_local","present");true' >"$OUT/cu-set.json" 2>"$OUT/cu-set.err"
    CU_SET_RC=$?
    ok cu-daemon-set-state "$CU_SET_RC" '[ "$CU_SET_RC" = 0 ]'
    cu_assert_state cu-daemon-state-before-close yes

    # Off-the-record contexts must not receive the default profile's cookies/storage.
    run_capture "$BIN" --port "$CU_CDP" context-new --url "$CU_URL"
    CTX_ID=$(printf '%s' "$LAST" | cut -f1)
    CTX_TAB=$(printf '%s' "$LAST" | cut -f2)
    run_capture "$BIN" --port "$CU_CDP" --tab "$CTX_TAB" eval 'JSON.stringify({session:document.cookie.includes("cu_session="),persistent:document.cookie.includes("cu_persistent="),local:localStorage.getItem("cu_local")==="present"})'
    CTX_STATE=$(printf '%s' "$LAST" | python3 -c 'import json,sys; v=json.load(sys.stdin); v=json.loads(v) if isinstance(v,str) else v; print("isolated" if not any(v[k] for k in ("session","persistent","local")) else "leaked")' 2>/dev/null)
    ok cu-isolated-context "$CTX_STATE" '[ "$CTX_STATE" = isolated ]'
    t_ok cu-isolated-context-close "$BIN" --port "$CU_CDP" context-close "$CTX_ID"
    cu_assert_state cu-default-untouched-by-context yes

    T0=$(now_ms)
    cu_stop_daemon
    M_CU_STOP=$(( $(now_ms) - T0 ))
    ok cu-daemon-graceful-stop "${CU_FORCE_KILL:-1}" '[ "$CU_FORCE_KILL" = 0 ]'
    T0=$(now_ms)
    must cu_start_daemon
    M_CU_RESTART=$(( $(now_ms) - T0 ))
    cu_navigate cu-daemon-navigate-after-restart
    cu_assert_state cu-daemon-session-persistent-local-after-restart yes
    cu_stop_daemon
    ok cu-daemon-second-graceful-stop "${CU_FORCE_KILL:-1}" '[ "$CU_FORCE_KILL" = 0 ]'
    t_ok delete-cu-profile "$BIN" profile delete cuuser --yes
fi

# Registry mutation, no-secret output, no-follow deletion, and renamed data persistence.
t_ok rename-alpha "$BIN" profile rename alpha renamed
t_re default-follows-rename '^renamed$' "$BIN" profile current
t_re renamed-metadata '"user":"renamed"' "$BIN" --json profile show renamed
run_capture "$BIN" --json profile show renamed
if printf '%s' "$LAST" | grep -Eq 'cookie-marker-7451|local-marker-7451|idb-marker-7451|session-marker-7451'; then
    echo 'FAIL [no-secret-show] storage value appeared in profile show'; fail=$((fail + 1));
else pass=$((pass + 1)); fi
if grep -Eq 'cookie-marker-7451|local-marker-7451|idb-marker-7451|session-marker-7451' "$ROOT/renamed/profile.json"; then
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
rm -f "$ROOT/linkcase/data/Default/Preferences"
rmdir "$ROOT/linkcase/data/Default"
rmdir "$ROOT/linkcase/data"
ln -s "$VICTIM" "$ROOT/linkcase/data"
t_fail symlink-data-start 'not a real directory' "$BIN" --session "$DUP" start --user linkcase --port "$P2" --no-gov
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
echo "== profile metrics: start_ms=$M_START stop_ms=$M_STOP reopen_start_ms=$M_FIRST_REOPEN restore_ms=$M_RESTORE registry_rss_kb=$PROFILE_RSS binary_bytes=$BINARY_BYTES cu_stop_ms=$M_CU_STOP cu_restart_ms=$M_CU_RESTART"
[ "$fail" = 0 ]
