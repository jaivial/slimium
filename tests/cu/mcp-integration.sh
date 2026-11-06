#!/usr/bin/env bash
# Native cu MCP against the isolated, already deployed cu-slim only:
# HTTP 8790, CDP 9390, ~/.local/share/cu-slim. Never touches the user's cu.
set -e
cd "$(dirname "$0")/../.."
CU_SLIM=$HOME/.local/bin/cu-slim
[ -x "$CU_SLIM" ] || { echo "missing $CU_SLIM (already deployed cu-slim required)" >&2; exit 1; }
export CU_SLIM_HOME=$HOME/.local/share/cu-slim
export CU_SLIM_PORT=8790
export CU_SLIM_CDP_PORT=9390
TMP=$(mktemp -d /tmp/cu-slim-mcp-test.XXXXXX)
DAEMON_PID=
cleanup() {
  if [ -n "$DAEMON_PID" ]; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    for i in $(seq 1 50); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID" 2>/dev/null || true
  fi
  [ -n "$KEEP" ] || rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Do not let a CLI status probe auto-start an unrecorded daemon. Start it
# ourselves only when 8790 is closed, and retain the exact PID for cleanup.
if python3 - <<'PY'
import socket,sys
s=socket.socket();s.settimeout(.2)
r=s.connect_ex(('127.0.0.1',8790));s.close();sys.exit(0 if r==0 else 1)
PY
then
  echo "# using the already running isolated cu-slim daemon"
else
  setsid "$CU_SLIM" daemon >"$TMP/daemon.log" 2>&1 </dev/null &
  DAEMON_PID=$!
  echo "# started isolated cu-slim daemon pid $DAEMON_PID"
  for i in $(seq 1 200); do
    if python3 - <<'PY'
import socket,sys
s=socket.socket();s.settimeout(.1)
r=s.connect_ex(('127.0.0.1',8790));s.close();sys.exit(0 if r==0 else 1)
PY
    then break; fi
    kill -0 "$DAEMON_PID" 2>/dev/null || { cat "$TMP/daemon.log" >&2; exit 1; }
    sleep 0.1
  done
fi

python3 - "$CU_SLIM" <<'PY'
import json, os, pathlib, select, subprocess, sys, time
cu = sys.argv[1]
env = os.environ.copy()
passed = 0

def check(value, name):
    global passed
    if not value:
        raise AssertionError(name)
    passed += 1
    print(f"ok {passed} {name}", flush=True)

for _ in range(200):
    diag = json.loads(subprocess.check_output([cu, "diagnostics"], env=env, text=True, timeout=20))
    if (diag.get("session") or {}).get("owner") and (diag.get("browser") or {}).get("resolved"):
        break
    time.sleep(0.1)
else:
    raise TimeoutError("cu-slim browser diagnostics did not become ready")
browser = diag["browser"]
check(browser["requested"].endswith("/slimium-chrome"), "cu-slim requests slimium-chrome")
check(pathlib.Path(browser["resolved"]).samefile(pathlib.Path.home() / ".local/bin/slimium"),
      "cu-slim browser resolves to the real slimium binary")
owner = diag["session"]["owner"]
check(owner["port"] == 8790 and owner["cdp_port"] == 9390, "isolated HTTP/CDP ports")

class Client:
    def __init__(self, label):
        self.label = label
        self.p = subprocess.Popen([cu, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env)
        self.i = 0
    def request(self, method, params=None):
        self.i += 1
        msg = {"jsonrpc":"2.0", "id":self.i, "method":method}
        if params is not None: msg["params"] = params
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")) + "\n"); self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], 30)
        if not ready: raise TimeoutError(self.label)
        line = self.p.stdout.readline()
        value = json.loads(line)
        check(value["id"] == self.i and value["jsonrpc"] == "2.0", self.label + " JSON-RPC " + method)
        return value
    def call(self, name, args=None):
        r = self.request("tools/call", {"name":name, "arguments":args or {}})
        check("error" not in r and not r["result"].get("isError"), self.label + " tool " + name)
        return r["result"]
    def text(self, name, args=None):
        return "\n".join(x.get("text", "") for x in self.call(name, args)["content"] if x.get("type") == "text")
    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try: self.p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.p.terminate(); self.p.wait(timeout=3)
        check(self.p.returncode == 0, self.label + " clean shutdown")

one = Client("cu-one")
two = Client("cu-two")
try:
    init = one.request("initialize", {"protocolVersion":"2025-03-26", "capabilities":{},
                                      "clientInfo":{"name":"slimium-cu-test","version":"1"}})
    check(init["result"]["serverInfo"]["name"] == "cu", "native cu MCP serverInfo")
    one.p.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n'); one.p.stdin.flush()
    tools = one.request("tools/list", {})["result"]["tools"]
    check({"status","navigate","snapshot","text","tabs"}.issubset({t["name"] for t in tools}), "cu MCP tools/list")
    status = one.text("status")
    check('"running":true' in status, "cu daemon status through MCP")

    one.text("navigate", {"url":"data:text/html,<title>CuOne</title><h1>CU_MCP_ONE_OK</h1>"})
    two_init = two.request("initialize", {"protocolVersion":"2025-03-26"})
    check(two_init["result"]["protocolVersion"] == "2025-03-26", "second cu MCP client initialize")
    two.text("navigate", {"url":"data:text/html,<title>CuTwo</title><h1>CU_MCP_TWO_OK</h1>"})
    check("CU_MCP_ONE_OK" in one.text("text"), "first MCP session retains its page")
    check("CU_MCP_TWO_OK" in two.text("text"), "second MCP session has a distinct page")
    snapshot = one.text("snapshot")
    check("CU_MCP_ONE_OK" in snapshot and "CuOne" in snapshot, "cu snapshot uses the same session")
    tabs = one.text("tabs")
    check("CuOne" in tabs or "CU_MCP_ONE_OK" in tabs, "cu tabs includes session page")
finally:
    one.close(); two.close()
print(f"# {passed} checks, 0 failures", flush=True)
PY
