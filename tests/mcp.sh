#!/usr/bin/env bash
# Isolated real-browser MCP test. Python is test-only; slimium itself is C11.
set -e
cd "$(dirname "$0")/.."
BIN=./slimium
[ -z "$SLIM_BIN" ] || BIN=$SLIM_BIN
PORT=9530
[ -z "$SLIM_MCP_TEST_PORT" ] || PORT=$SLIM_MCP_TEST_PORT
TMP=$(mktemp -d /tmp/slimium-mcp-test.XXXXXX)
SES=mcp-test-$$
export SLIM_STATE_DIR=$TMP/state
mkdir -p "$SLIM_STATE_DIR"
cleanup() {
  "$BIN" --session "$SES" stop >/dev/null 2>&1 || true
  [ -n "$KEEP" ] || rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
export SLIM_MCP_LOG=$TMP/mcp.log
# MCP must suppress CDP debug traces because they can contain form input.
export SLIM_DEBUG=1
python3 - "$BIN" "$SES" "$PORT" "$TMP" <<'PY'
import json, os, pathlib, select, shlex, socket, statistics, struct
import subprocess, sys, time

bin_path = str(pathlib.Path(sys.argv[1]).resolve())
session, port_s, tmp_s = sys.argv[2:5]
port = int(port_s)
tmp = pathlib.Path(tmp_s)
fixture = (pathlib.Path.cwd() / "tests/fixtures/form.html").resolve()
passed = 0

def check(condition, name):
    global passed
    if not condition:
        raise AssertionError(name)
    passed += 1
    print(f"ok {passed} {name}", flush=True)

probe = socket.socket()
try:
    probe.bind(("127.0.0.1", port))
finally:
    probe.close()

class Client:
    def __init__(self):
        env = os.environ.copy()
        self.p = subprocess.Popen(
            [bin_path, "--session", session, "mcp", "--profile", "balanced"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, bufsize=0)
        self.next_id = 0
        self.lines = 0

    def read(self):
        ready, _, _ = select.select([self.p.stdout], [], [], 40)
        if not ready:
            raise TimeoutError("MCP response timeout")
        line = self.p.stdout.readline()
        if not line:
            raise EOFError(f"MCP closed stdout; stderr={self.p.stderr.read()!r}")
        value = json.loads(line.decode("utf-8"))
        if not isinstance(value, dict) or value.get("jsonrpc") != "2.0":
            raise AssertionError(f"stdout noise or bad JSON-RPC: {line!r}")
        self.lines += 1
        return value

    def request(self, method, params=None, ident=None):
        if ident is None:
            self.next_id += 1
            ident = self.next_id
        msg = {"jsonrpc": "2.0", "id": ident, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")).encode() + b"\n")
        self.p.stdin.flush()
        response = self.read()
        check(response.get("id") == ident, f"JSON-RPC id {method}")
        return response

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")).encode() + b"\n")
        self.p.stdin.flush()

    def framed(self, msg):
        body = json.dumps(msg, separators=(",", ":")).encode()
        self.p.stdin.write(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
        self.p.stdin.flush()
        return self.read()

    def call(self, name, args=None, count=True):
        response = self.request("tools/call", {"name": name, "arguments": args or {}})
        if count:
            check("error" not in response, f"tool protocol {name}")
        return response["result"]

    def payload(self, name, args=None, count=True):
        result = self.call(name, args, count=count)
        if count:
            check(not result.get("isError"), f"tool success {name}")
        text = result["content"][0]["text"]
        value = json.loads(text)
        if count:
            check(value.get("ok") is True, f"tool payload {name}")
        return value, result

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:
                self.p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.p.terminate()
                try:
                    self.p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.p.kill(); self.p.wait()
        return self.p.stdout.read(), self.p.stderr.read(), self.p.returncode

client = Client()
stopped = False
try:
    init = client.request("initialize", {
        "protocolVersion": "2024-11-05", "capabilities": {},
        "clientInfo": {"name": "slimium-test", "version": "1"}})
    check(init["result"]["protocolVersion"] == "2024-11-05", "initialize 2024-11-05")
    check(init["result"]["serverInfo"]["name"] == "slimium", "serverInfo")
    client.notify("notifications/initialized")
    ping = client.request("ping", ident="string-ping")
    check(ping["result"] == {} and ping["id"] == "string-ping", "ping and initialized notification silence")

    tools = client.request("tools/list", {})["result"]["tools"]
    names = {t["name"] for t in tools}
    expected = {"browser_status", "browser_start", "browser_stop", "tabs", "open", "close",
                "switch", "navigate", "snapshot", "click", "fill", "type", "press", "wait",
                "eval", "text", "html", "screenshot", "viewport", "record", "profile",
                "slim_cli", "slim_batch", "slim_cdp"}
    check(names == expected, "tools/list complete surface")
    for tool in tools:
        schema = tool["inputSchema"]
        if schema.get("type") != "object" or schema.get("additionalProperties") is not False:
            raise AssertionError(f"bad schema {tool['name']}")
    check(True, "all schemas closed objects")
    eval_tool = next(t for t in tools if t["name"] == "eval")
    check(eval_tool["inputSchema"]["required"] == ["expression"], "required schema fields")

    init2 = client.request("initialize", {"protocolVersion": "2025-03-26"})
    check(init2["result"]["protocolVersion"] == "2025-03-26", "initialize 2025-03-26")
    init3 = client.request("initialize", {"protocolVersion": "2099-01-01"})
    check(init3["result"]["protocolVersion"] == "2025-03-26", "sane version negotiation")
    framed = client.framed({"jsonrpc": "2.0", "id": "content-length", "method": "ping"})
    check(framed["id"] == "content-length" and framed["result"] == {}, "Content-Length request framing")

    client.p.stdin.write(b"{not-json\n"); client.p.stdin.flush()
    check(client.read()["error"]["code"] == -32700, "malformed JSON parse error")
    client.p.stdin.write(b"[]\n"); client.p.stdin.flush()
    check(client.read()["error"]["code"] == -32600, "JSON-RPC batch rejected")
    bad = client.request("ping", ident=4.5)
    check(bad["result"] == {}, "numeric JSON-RPC id preserved")
    bad = client.request("does/not/exist", ident="missing")
    check(bad["error"]["code"] == -32601, "method not found")
    bad = client.request("tools/call", {"name": "eval", "arguments": {"bogus": 1}})
    check(bad["error"]["code"] == -32602, "additional property rejected")
    bad = client.request("tools/call", {"name": "no_such_tool", "arguments": {}})
    check(bad["error"]["code"] == -32602, "unknown tool rejected")
    client.notify("notifications/unknown", {"ignored": True})
    after_notification = client.request("ping", ident="after-notification")
    check(after_notification["id"] == "after-notification", "unknown notification has no response")
    client.notify("notifications/cancelled", {"requestId": "cancel-me"})
    cancelled = client.request("tools/call", {"name": "slim_cli", "arguments": {"argv": ["version"]}}, ident="cancel-me")
    check(cancelled["error"]["code"] == -32800, "best-effort cancellation")

    for dangerous in ("mcp", "pipe", "batch", "chrome", "gov"):
        result = client.call("slim_cli", {"argv": [dangerous]})
        text = result["content"][0]["text"]
        check(result["isError"] is True and "not available" in text, f"reject {dangerous} recursion/process entry")
    result = client.call("slim_cli", {"argv": ["shot", "-"]})
    check(result["isError"] is True and "JSON-RPC" in result["content"][0]["text"], "reject binary stdout screenshot")
    result = client.call("slim_cli", {"argv": ["eval", "-"]})
    check(result["isError"] is True, "reject MCP stdin consumption")

    started, _ = client.payload("browser_start", {
        "port": port, "userDataDir": str(tmp / "udd"), "noGovernor": True})
    check(started["port"] == port and started["profile"] == "balanced", "browser_start assigned port/profile")
    status, _ = client.payload("browser_status")
    check(status["alive"] is True and status["session"] == session, "browser_status persistent session")

    nav, _ = client.payload("navigate", {"url": str(fixture)})
    check(nav["title"] == "Form fixture", "navigate real file page")
    value, _ = client.payload("eval", {"expression": "new Promise(r => setTimeout(() => r(42), 20))"})
    check(value["value"] == 42, "eval awaits promise")
    secret = "mcp-test-input-not-a-real-secret"
    client.payload("fill", {"selector": "#name", "text": secret})
    value, _ = client.payload("eval", {"expression": "document.querySelector('#name').value"})
    check(value["value"] == secret, "fill round trip")
    client.payload("type", {"selector": "#name", "text": "X"})
    value, _ = client.payload("eval", {"expression": "document.querySelector('#name').value"})
    check(value["value"] == secret + "X", "type appends key events")
    client.payload("press", {"key": "Control+a"})
    value, _ = client.payload("eval", {"expression": "lastKey"})
    check("ctrl" in value["value"], "press modifier")
    client.payload("click", {"selector": "#clicks"})
    value, _ = client.payload("text", {"selector": "#clicks"})
    check("clicks 1" in value["value"], "click real mouse event")
    client.payload("wait", {"selector": "#h", "visible": True, "timeoutMs": 2000})
    value, _ = client.payload("html", {"selector": "h1"})
    check(value["value"] == '<h1 id="h">Form</h1>', "html selector")
    value, _ = client.payload("slim_cli", {"argv": ["eval", "6*7"]})
    check(value["value"] == 42, "generic slim_cli")
    value, _ = client.payload("slim_cdp", {
        "method": "Runtime.evaluate", "target": "page",
        "params": {"expression": "6*7", "returnByValue": True}})
    check(value["result"]["result"]["value"] == 42, "generic slim_cdp")
    batch, _ = client.payload("slim_batch", {"commands": [["eval", "1+2"], ["text", "#h"]]})
    check(len(batch["results"]) == 2 and batch["results"][0]["result"]["value"] == 3, "generic slim_batch")
    failed, failed_result = client.payload("slim_batch", {
        "commands": [["eval", "throw new Error('boom')"], ["eval", "99"]], "stopOnError": True}, count=False)
    check(failed_result["isError"] is True and failed["ok"] is False and len(failed["results"]) == 1,
          "batch item error and stopOnError")
    runtime_error = client.call("eval", {"expression": "throw new Error('boom')"})
    check(runtime_error["isError"] is True and "boom" in runtime_error["content"][0]["text"], "tool isError useful text")

    snapshot = client.call("snapshot")
    if snapshot["isError"]:
        check("unknown command" in snapshot["content"][0]["text"], "snapshot generic pre-merge fallback")
    else:
        check(bool(snapshot["content"][0]["text"]), "snapshot merged command")

    client.payload("viewport", {"width": 640, "height": 480})
    value, _ = client.payload("eval", {"expression": "[innerWidth,innerHeight]"})
    check(value["value"] == [640, 480], "viewport applied persistently")
    for fmt, suffix in (("png", "png"), ("jpeg", "jpg"), ("webp", "webp")):
        path = tmp / f"shot.{suffix}"
        shot, _ = client.payload("screenshot", {"path": str(path), "format": fmt})
        check(shot["file"] == str(path) and shot["bytes"] > 1000 and
              shot["width"] == 640 and shot["height"] == 480 and path.is_file(),
              f"screenshot {fmt} file and dimensions")
        check("base64" not in json.dumps(shot).lower(), f"screenshot {fmt} not inline")
    png = (tmp / "shot.png").read_bytes()
    check(struct.unpack(">II", png[16:24]) == (640, 480), "PNG dimensions match file")
    client.payload("viewport", {"reset": True})

    tabs, _ = client.payload("tabs")
    first = next(t["id"] for t in tabs["tabs"] if t["current"])
    opened, _ = client.payload("open", {"url": "data:text/html,<title>MCP Two</title><h1>Two</h1>"})
    second = opened["id"]
    scoped, _ = client.payload("slim_cli", {"argv": ["--tab", first, "eval", "document.title"]})
    check(scoped["value"] == "Form fixture", "scoped tab override")
    current, _ = client.payload("eval", {"expression": "document.title"})
    check(current["value"] == "MCP Two", "current tab survives scoped override")
    client.payload("switch", {"id": first})
    current, _ = client.payload("eval", {"expression": "document.title"})
    check(current["value"] == "Form fixture", "switch persists current tab")
    client.payload("close", {"id": second})
    tabs, _ = client.payload("tabs")
    check(all(t["id"] != second for t in tabs["tabs"]), "close tab")

    client.payload("navigate", {"url": str(fixture)})
    workload = []
    for i in range(100):
        if i % 20 == 0:
            workload.append(("click", "#clicks"))
        elif i % 10 == 0:
            workload.append(("text", "#h"))
        else:
            workload.append(("eval", f"(({i}*7)%13)+document.querySelectorAll('input').length"))

    mcp_times = []
    t0 = time.perf_counter()
    for command, arg in workload:
        begin = time.perf_counter()
        if command == "eval":
            value, _ = client.payload("eval", {"expression": arg}, count=False)
        elif command == "text":
            value, _ = client.payload("text", {"selector": arg}, count=False)
        else:
            value, _ = client.payload("click", {"selector": arg}, count=False)
        if not value.get("ok"):
            raise AssertionError(("MCP benchmark command", command, value))
        mcp_times.append(time.perf_counter() - begin)
    mcp_total = time.perf_counter() - t0
    check(len(mcp_times) == 100, "100 MCP calls over one process/connection")

    def rss_kb(pid):
        with open(f"/proc/{pid}/status", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return 0
    mcp_rss = rss_kb(client.p.pid)

    clean_env = os.environ.copy()
    clean_env.pop("SLIM_DEBUG", None)
    clean_env.pop("SLIM_MCP_LOG", None)
    cli_times = []
    for command, arg in workload:
        begin = time.perf_counter()
        run = subprocess.run([bin_path, "--session", session, command, arg],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=clean_env)
        if run.returncode:
            raise AssertionError(("CLI benchmark command", command, run.returncode))
        cli_times.append(time.perf_counter() - begin)
    cli_total = sum(cli_times)
    check(len(cli_times) == 100, "100 separate CLI calls")

    batch_times = []
    batch_start = time.perf_counter()
    bp = subprocess.Popen([bin_path, "--session", session, "batch"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=clean_env, bufsize=1)
    previous = batch_start
    for command, arg in workload:
        bp.stdin.write(shlex.join([command, arg]) + "\n")
    bp.stdin.flush()
    for _ in workload:
        line = bp.stdout.readline()
        if not line or not json.loads(line).get("ok"):
            raise AssertionError(("batch benchmark", line, bp.stderr.read()))
        now = time.perf_counter()
        batch_times.append(now - previous)
        previous = now
    bp.stdin.close()
    batch_total = time.perf_counter() - batch_start
    batch_err = bp.stderr.read()
    if bp.wait(timeout=10) or batch_err:
        raise AssertionError(("batch benchmark exit", bp.returncode, batch_err))
    check(len(batch_times) == 100, "100 commands in one CLI batch")

    def report(mode, times, total, processes):
        median_ms = statistics.median(times) * 1000
        result = {"mode": mode, "calls": 100, "processes": processes,
                  "median_ms": round(median_ms, 3), "total_ms": round(total * 1000, 1),
                  "calls_per_s": round(100 / total, 1),
                  "median_calls_per_s": round(1000 / median_ms, 1)}
        print("BENCH " + json.dumps(result, separators=(",", ":")), flush=True)
        return result
    mcp_bench = report("mcp", mcp_times, mcp_total, 1)
    cli_bench = report("cli", cli_times, cli_total, 100)
    batch_bench = report("cli-batch", batch_times, batch_total, 1)
    print("BENCH " + json.dumps({"connection_processes": {"mcp": 1, "cli": 100, "cli-batch": 1},
                                 "mcp_rss_kb": mcp_rss}, separators=(",", ":")), flush=True)
    check(mcp_bench["calls"] == cli_bench["calls"] == batch_bench["calls"] == 100, "benchmark workloads equal")

    client.payload("browser_stop")
    stopped = True
    remaining_out, stderr, rc = client.close()
    check(rc == 0, "clean MCP shutdown on stdin EOF")
    check(remaining_out == b"", "no trailing stdout noise")
    check(stderr == b"", "no CDP debug/stderr leak")
    log = pathlib.Path(os.environ["SLIM_MCP_LOG"]).read_text()
    check("tool eval" in log and "initialize" in log, "optional MCP log enabled")
    check(secret not in log and secret not in stderr.decode(), "input values absent from logs")
finally:
    if not stopped:
        try:
            client.payload("browser_stop", count=False)
        except Exception:
            pass
    if client.p.poll() is None:
        client.close()
    subprocess.run([bin_path, "--session", session, "stop"], env=os.environ.copy(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)

print(f"# {passed} checks, 0 failures", flush=True)
PY
