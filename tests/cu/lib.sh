# Shared helpers for tests/cu (sourced by run.sh and ram.sh). POSIX-ish bash.
# Everything here only ever signals PIDs this script started itself.

CU_TESTS_DIR=${CU_TESTS_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
CU_SLIM=${CU_SLIM:-$HOME/.local/bin/cu-slim}
STANDIN=$CU_TESTS_DIR/slimium-chrome-standin.sh

# The browser cu-slim should run: the real wrapper when installed, else the stand-in.
slim_browser() {
  if [ -n "${SLIMIUM_CHROME:-}" ]; then echo "$SLIMIUM_CHROME"
  elif [ -x "$HOME/.local/bin/slimium-chrome" ]; then echo "$HOME/.local/bin/slimium-chrome"
  else echo "$STANDIN"; fi
}

# Fingerprint of the user's own cu: daemon PIDs, their browser PIDs, and the
# loopback listeners of the canonical daemon (8810 HTTP / 9362 CDP; cu walks
# upward from those when busy, so the whole walked ranges are recorded).
user_cu_fingerprint() {
  local pids p
  pids=$(pgrep -f "^$HOME/.cargo/bin/cu daemon" | sort -n | tr '\n' ' ')
  echo "daemon=[$pids]"
  for p in $pids; do echo "children($p)=[$(pgrep -P "$p" | sort -n | tr '\n' ' ')]"; done
  ss -ltnH 2>/dev/null | awk '{print $4}' | grep -E '^127\.0\.0\.1:(88(1[0-9]|2[0-9]|3[0-4])|93(6[2-9]|7[0-9]|8[0-6]))$' | sort | tr '\n' ' '
  echo
}

# All descendants of a PID (inclusive), from /proc/<pid>/task/*/children.
tree_pids() {
  local queue=("$1") out=() p c
  while [ ${#queue[@]} -gt 0 ]; do
    p=${queue[0]}; queue=("${queue[@]:1}")
    [ -d /proc/$p ] || continue
    out+=("$p")
    for c in $(cat /proc/$p/task/*/children 2>/dev/null); do queue+=("$c"); done
  done
  echo "${out[@]}"
}

# PSS / RSS (kB) and process count of a tree: "pss_kb rss_kb procs".
tree_mem() {
  local pids p
  pids=$(tree_pids "$1")
  for p in $pids; do cat /proc/$p/smaps_rollup 2>/dev/null; done |
    awk -v n="$(echo $pids | wc -w)" '/^Pss:/{pss+=$2} /^Rss:/{rss+=$2} END{printf "%d %d %d\n", pss, rss, n}'
}

# The browser PID of a cu daemon: its child listening on (or launched with) the CDP port.
browser_pid_of() { # $1 daemon pid, $2 cdp port
  local c
  for c in $(pgrep -P "$1"); do
    if tr '\0' ' ' </proc/$c/cmdline 2>/dev/null | grep -q -- "--remote-debugging-port=$2"; then echo "$c"; return 0; fi
  done
  return 1
}

# start_instance NAME HOME HTTP CDP BROWSER: an isolated cu-slim daemon.
# Sets INST_PID_<NAME>, INST_BROWSER_<NAME>. Waits until the browser answers.
start_instance() {
  local name=$1 home=$2 port=$3 cdp=$4 browser=$5 pid i
  if ss -ltnH | awk '{print $4}' | grep -qE "^127\.0\.0\.1:($port|$cdp)$"; then
    echo "port $port or $cdp already in use; refusing to start $name" >&2; return 1
  fi
  mkdir -p "$home"
  CU_SLIM_HOME=$home CU_SLIM_PORT=$port CU_SLIM_CDP_PORT=$cdp CU_SLIM_BROWSER=$browser \
    setsid "$CU_SLIM" daemon >"$home/daemon.out" 2>&1 </dev/null &
  pid=$!
  eval "INST_PID_$name=$pid"
  for i in $(seq 1 200); do
    if CU_SLIM_HOME=$home "$CU_SLIM" status >/dev/null 2>&1; then break; fi
    sleep 0.1
  done
  # The daemon answers before its browser is up; a page call waits for it.
  CU_SLIM_HOME=$home "$CU_SLIM" tabs >/dev/null 2>&1 || { echo "$name: browser did not come up" >&2; cat "$home/daemon.out" >&2; return 1; }
  eval "INST_BROWSER_$name=$(browser_pid_of "$pid" "$cdp")"
}

# stop_instance NAME: SIGTERM the daemon we started (it closes its browser),
# then make sure the browser tree we started is gone.
stop_instance() {
  local name=$1 pid browser i
  eval "pid=\${INST_PID_$name:-}"; eval "browser=\${INST_BROWSER_$name:-}"
  [ -n "$pid" ] || return 0
  kill -TERM "$pid" 2>/dev/null
  for i in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  if [ -n "$browser" ]; then
    for i in $(seq 1 50); do kill -0 "$browser" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$browser" 2>/dev/null; then
      local t; t=$(tree_pids "$browser"); kill -TERM $t 2>/dev/null; sleep 1; kill -KILL $t 2>/dev/null
    fi
  fi
  eval "INST_PID_$name="
}

# Fixture HTTP server (python3 http.server) on 127.0.0.1:$1. Sets FIXTURE_PID.
start_fixtures() {
  local port=$1 i
  python3 -m http.server "$port" --bind 127.0.0.1 -d "$CU_TESTS_DIR/fixtures" >/dev/null 2>&1 &
  FIXTURE_PID=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$port/index.html" >/dev/null && return 0; sleep 0.1; done
  echo "fixture server did not start on $port" >&2; return 1
}
stop_fixtures() { [ -n "${FIXTURE_PID:-}" ] && kill "$FIXTURE_PID" 2>/dev/null; FIXTURE_PID=; }

median() { sort -n | awk '{a[NR]=$1} END{if(NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2)}'; }
