# slimium MCP server

`slimium mcp` is a dependency-free stdio [Model Context Protocol](https://modelcontextprotocol.io)
server for the same C11 command dispatcher and DevTools connection used by the CLI. It is a
server in the slimium process, not a Python/Node adapter and not a wrapper around cu.

## Start and client configuration

```sh
slimium --session default mcp
# Optional default memory profile for browser_start:
slimium --session agent mcp --profile balanced
```

`--profile` here is the existing **memory** profile (`off|balanced|max`), not a named
website-login profile. MCP does not invent named user-profile behavior. The `profile` tool only
forwards arguments to a profile command if another merged module provides one.

Generic MCP client JSON (use the installed absolute path where possible):

```json
{
  "mcpServers": {
    "slimium": {
      "command": "/home/jaime/.local/bin/slimium",
      "args": ["--session", "default", "mcp"]
    }
  }
}
```

Examples for command-line client registration:

```sh
claude mcp add slimium -- slimium --session default mcp
codex mcp add slimium -- slimium --session default mcp
pi mcp add slimium -- slimium --session default mcp
```

No browser is launched merely by starting MCP. Call `browser_start`, or use an existing slimium
session. Closing stdin shuts down only the MCP process; it does **not** stop the browser.

## Wire protocol

- JSON-RPC 2.0 over stdio, one compact JSON object per line in both directions.
- Requests framed as `Content-Length: N\r\n\r\n<body>` are also accepted. Responses always use
  newline-delimited MCP stdio framing.
- Maximum input message: 16 MiB. JSON-RPC batch arrays are rejected with `-32600`.
- Protocol versions `2024-11-05` and `2025-03-26` are returned unchanged when requested;
  another version negotiates to `2025-03-26`.
- Methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`.
  Notifications, including unknown notifications, produce no response.
- String and numeric ids are preserved. Errors include `-32700` (parse), `-32600` (invalid
  request), `-32601` (method), `-32602` (params/unknown tool), and tool results with
  `isError:true` plus actionable text.
- `notifications/cancelled` is remembered for a later matching request id (`-32800`). The server
  is deliberately sequential, so cancellation cannot interrupt a DevTools call already running.

Stdout carries only JSON-RPC. Set `SLIM_MCP_LOG=1` for event logging to stderr, or
`SLIM_MCP_LOG=/path/to/log` for a mode-0600 append log. Events contain method/tool names and
status only, never argv, field input, page results, cookies, or tokens. MCP disables
`SLIM_DEBUG` internally because raw CDP traces can contain form input. Explicitly requested
page/cookie/eval results are still returned to the MCP client, as they are by the CLI.

## Persistent execution model

All tools use one `ctx_t` and one lazily established browser WebSocket. Commands run through
`cli_invoke` in-process; there is no fork or new slimium process per call. The session state and
its current target/flattened CDP session are retained between calls. A per-call `--tab` override
is detached and restored afterward. Temporary `--session`/`--port` overrides are scoped to one
`slim_cli`/`slim_batch` item, after which the original session is restored. Starting, stopping,
or attaching invalidates stale cached state so the next call reloads the right session.

Tool responses have one compact text content block. Its text is a JSON envelope such as
`{"ok":true,"value":42,"type":"number"}`. Command failures use `isError:true` and
`{"ok":false,"error":"..."}`. Large page results are not duplicated into image or base64 blocks.

## Tools

| tool | required input | optional input / behavior |
|---|---|---|
| `browser_status` | — | session, browser, profile, tabs, process-tree memory |
| `browser_start` | — | `port`, memory `profile`, `userDataDir`, `headful`, `window`, `noGovernor` |
| `browser_stop` | — | stop only the selected session's own browser; attached sessions are detached |
| `tabs` | — | list page targets and current tab |
| `open` | — | `url`, `context`, `wait`, `background`; makes the new tab current |
| `close` | — | `id`; omit for current tab |
| `switch` | `id` | id or unique prefix; persists current tab |
| `navigate` | `url` | `wait=load|dom|none`, `tab` |
| `snapshot` | — | `args:string[]` forwarded to merged `snapshot` command |
| `click` | `selector` | `tab`; CSS/XPath, real mouse events |
| `fill` | `selector`, `text` | `tab`; replace field contents |
| `type` | `selector`, `text` | `tab`; key events per character |
| `press` | `key` | `tab`; modifiers such as `Control+a` |
| `wait` | `selector` | `visible`, `gone`, `timeoutMs`, `tab` |
| `eval` | `expression` | `tab`; awaits promises, returns values by JSON |
| `text` / `html` | — | `selector`, `tab` |
| `screenshot` | `path` | `full`, `selector`, `format=png|jpeg|webp`, `quality`, `tab` |
| `viewport` | width+height or `reset:true` | `scale`, `mobile`, `tab` |
| `record` | — | `args:string[]` forwarded to merged media command |
| `profile` | — | `args:string[]` forwarded to merged profile command |
| `slim_cli` | `argv:string[]` | any registered non-process-entry command and global options |
| `slim_batch` | `commands:string[][]` | `stopOnError`; ordered results over the same context |
| `slim_cdp` | `method` | object `params`, `target=auto|browser|page`, `tab` |

Every `inputSchema` is an object with `additionalProperties:false` and explicit required fields.
`snapshot`, `record`, and `profile` intentionally use generic argument arrays so their merged
modules define the actual command syntax. Before those modules exist, calling one returns a
useful `unknown command` tool error; no MCP source change is needed after merging. Other newly
registered commands are immediately available through `slim_cli` and `slim_batch`.

`screenshot` writes the requested file and returns `file`, `bytes`, `format`, `width`, and
`height` for PNG, JPEG, and WebP. It does not return inline base64. Use `slim_cdp` directly if an
inline DevTools response is explicitly required.

### Generic-tool safety

`slim_cli` and batch items reject `mcp`, `pipe`, `batch`, `chrome`, and `gov`: these would recurse,
consume MCP stdin, replace the process, or start an unintended foreground process. `eval -` is
rejected because it would consume JSON-RPC stdin; `shot -` and `pdf -` are rejected because they
would write binary data to JSON-RPC stdout. Browser lifecycle commands remain available through
`browser_start`/`browser_stop` (and the generic interface) with the existing slimium safety rules.

## Tests and measurements

```sh
make -j2
tests/mcp.sh
tests/cu/mcp-integration.sh
```

`tests/mcp.sh` uses a Python-stdlib test client (Python is not used by the binary), fresh /tmp
state/profile, and only CDP 9530 by default (override `SLIM_MCP_TEST_PORT` within the mcp track's
9530-9549 assignment). It checks initialize/list, both protocol versions, both framings, ids,
malformed requests, notifications, cancellation, closed schemas, recursion/stdout guards, real
browser lifecycle/navigation/input/eval/CDP/batch, current-tab behavior, screenshots and logs,
clean EOF, and the workloads below. Current result: **325 checks, 0 failures**.

Representative 2026-11-06 run on Chromium 156, 90 eval + 5 text + 5 click operations (100 total;
identical workload in each mode):

| mode | processes / connections | median per call | total for 100 | overall calls/s | 1000 / median ms |
|---|---:|---:|---:|---:|---:|
| MCP | 1 / 1 | 0.249 ms | 85.0 ms | 1177.0 | 4015.5 |
| separate CLI | 100 / 100 | 0.994 ms | 163.6 ms | 611.3 | 1006.0 |
| CLI batch | 1 / 1 | 0.109 ms | 82.4 ms | 1213.8 | 9173.7 |

The MCP process was 2376 KiB RSS after the browser workloads. Batch remains useful when input is
already line-oriented; MCP avoids both per-command process setup and repeated CDP attach setup
while adding structured protocol access. Benchmark timing includes JSON/Python test-client round
trips for MCP and process spawn for each separate CLI invocation; it is not a Chromium rendering
benchmark.

## Limitations

- Calls are serialized; there is no parallel tool execution or interruption of an in-flight CDP
  operation. A client timeout does not cancel the underlying browser action.
- Responses are newline-framed even when a request used `Content-Length` framing.
- No JSON-RPC request batch arrays, resources, prompts, roots, sampling, or tool-list change
  notifications are advertised.
- Generic output is text JSON rather than `structuredContent`. File tools return metadata instead
  of image content blocks.
- MCP does not auto-stop a session browser on exit, provide named website-login profiles, or
  change the low-RAM governor. Start/governor behavior remains the existing command behavior.
