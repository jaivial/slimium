# Chrome DevTools Protocol

slimium exposes the protocol of the **running Chromium engine**, rather than a compiled-in
list of supported commands. Discovery reads `GET /json/protocol` on the selected session (or
`--port`) every time it is requested. Method calls use Chromium's browser WebSocket and flat
target sessions.

## Coverage claim

Every method advertised by that live schema can be passed to `slimium cdp`, with an arbitrary
JSON parameter object and an explicit browser, page, target, or existing flat-session route.
There is no hand-maintained method whitelist and no fallback implementation in slimium.
Consequently, `protocol coverage` reports `callable_methods == methods` and zero listing
omissions.

This is an **invocation coverage** claim, not a promise that every operation succeeds:

- The engine, target type, operating system, headless/headful mode, and current state determine
  actual support and behavior. Chromium can reject a schema-advertised call.
- Experimental and deprecated methods are passed through unchanged. Their schema flags are
  shown by discovery, but slimium does not stabilize their behavior.
- No method is emulated. Errors are Chromium's errors, and results are returned without
  truncating large JSON values.
- The default route is the current page except for known browser domains. Use `--target` when
  a domain is available on a different target; slimium never retries a possibly non-idempotent
  method on another target automatically.

`tests/protocol.sh` downloads the live schema and compares its complete ordered domain,
command, and event lists with both text and JSON discovery output. It does **not** execute
unknown or destructive methods merely to demonstrate coverage.

## Discovery

```sh
slimium protocol domains
slimium protocol methods                 # every Domain.method
slimium protocol methods Network        # one domain
slimium protocol events Runtime
slimium protocol show Runtime.evaluate  # required/optional parameters and flags
slimium protocol version                # /json/version
slimium protocol coverage               # live counts and invocation coverage
```

Text output is compact. `--json` preserves the complete live schema objects:

- `protocol domains`: `domains` is the unmodified live domain array.
- `protocol methods` and `protocol events`: each array entry is
  `{"domain":"Network","schema":{...}}`; `schema` is the unmodified method or event.
- `protocol show`: `schema` is the unmodified method/event and `kind` distinguishes them.
- `protocol version`: `version` is the unmodified `/json/version` object.

Examples with [jq](https://jqlang.github.io/jq/):

```sh
# Names of all required Runtime.evaluate parameters.
slimium --json protocol show Runtime.evaluate |
  jq -r '.schema.parameters[] | select(.optional != true) | .name'

# Experimental methods in one domain.
slimium --json protocol methods Network |
  jq -r '.methods[] | select(.schema.experimental == true) |
         .domain + "." + .schema.name'

# Count events per domain using plain-text output.
slimium protocol events | cut -d. -f1 | sort | uniq -c | sort -nr
```

## Generic method calls

```sh
# Browser target and current page (the default routes for these domains).
slimium cdp Browser.getVersion
slimium cdp Runtime.evaluate '{"expression":"document.title","returnByValue":true}'

# Parameters from a file or standard input.
slimium cdp Runtime.evaluate @evaluate.json
printf '%s' '{"expression":"6*7","returnByValue":true}' |
  slimium cdp Runtime.evaluate -

# Explicit target class, global current-tab override, or temporary target attachment.
slimium cdp Target.getTargets --target browser
slimium --tab TARGET_PREFIX cdp Page.captureSnapshot --target page
slimium cdp Runtime.evaluate @evaluate.json --target-id TARGET_PREFIX

# In a shared-connection caller (batch/cli_invoke), an existing flat session can be used:
slimium cdp Runtime.evaluate @evaluate.json --cdp-session EXISTING_SESSION_ID
```

`--target`, `--target-id`, and `--cdp-session` are mutually exclusive. `--target-id` resolves a
full target id or unique prefix, attaches temporarily, and detaches afterward. `--cdp-session`
refers to an existing session on the same browser WebSocket; an invalid/stale id is rejected by
Chromium. Standalone CLI invocations use separate WebSockets, so a session created by one
invocation is stale in the next; this option is useful to `batch`/`cli_invoke()` callers that
already share a connection. `--target-id` is the standalone cross-command option. Parameters must be one JSON object, supplied inline, as `@path`, or as `-` for stdin.

Plain output is the raw method `result`. With `--json`, it is nested under `result` in the
normal slimium response envelope. Neither form abridges nested values. The legacy
`--events MS` option remains available to collect events after a call.

## Event listener

```sh
# Terminal 1: enable Runtime if advertised, then collect one event.
slimium --json --timeout 10000 cdp-listen Runtime.consoleAPICalled --count 1

# Terminal 2:
slimium eval 'console.log("hello from CDP")'
```

`cdp-listen EVENT [--timeout MS] [--count N]` accepts the same three explicit route options.
It validates the event against the live schema and calls `Domain.enable` when that method is
advertised; `--no-enable` disables this convenience. `--count 0` collects until the timeout.
A timeout with no matching event is a successful empty collection. Plain output has one event
per line; JSON output is one normal response containing an `events` array with each method,
optional session id, and full parameters. Collection is bounded and emitted at command exit,
so `cli_invoke()` never writes directly to stdout.

## Thin convenience wrappers

These commands only construct parameters for the named real CDP methods. Generic `cdp`
remains the complete interface.

| CLI | CDP mapping |
| --- | --- |
| `permissions grant PERMISSION... [--origin O] [--context ID]` | `Browser.grantPermissions` |
| `permissions reset [--context ID]` | `Browser.resetPermissions` |
| `geolocation set LAT LON [ACCURACY_M]` / `geolocation clear` | `Emulation.setGeolocationOverride` / `Emulation.clearGeolocationOverride` |
| `throttle PRESET` | `Network.enable` + `Network.emulateNetworkConditions` |
| `cache on\|off` | `Network.enable` + `Network.setCacheDisabled` |
| `dialog accept [TEXT]` / `dialog dismiss` | `Page.handleJavaScriptDialog`, sent by the session keeper (the only session that saw the dialog open) |
| `dialog auto accept\|dismiss [--timeout MS] [--count N]` | with a keeper: temporary `all=` policy, counted in the dialog log; without one: `Page.enable`, `Page.javascriptDialogOpening`, `Page.handleJavaScriptDialog` |
| `dialog policy [SPEC]` / `dialogs` | keeper policy per dialog type (accept/dismiss/manual) and the dialog log; see docs/EMULATION.md |
| `script on\|off` | `Emulation.setScriptExecutionDisabled` |

Network throughput values are bytes/second, as required by CDP:

| preset | latency ms | download | upload |
| --- | ---: | ---: | ---: |
| `slow-3g` | 400 | 51,200 | 51,200 |
| `fast-3g` | 150 | 204,800 | 96,000 |
| `slow-4g` | 150 | 409,600 | 409,600 |
| `fast-4g` | 60 | 4,194,304 | 3,145,728 |
| `offline` | 0 | -1 | -1, offline=true |
| `off` | 0 | -1 | -1 |

Examples:

```sh
slimium permissions grant geolocation --origin https://example.com
slimium geolocation set 40.4168 -3.7038 25
slimium geolocation clear
slimium throttle slow-3g
slimium throttle off
slimium cache off
slimium script off && slimium script on

# Keep the auto-handler alive while another command opens alert(), prompt(), or confirm().
slimium --timeout 10000 dialog auto dismiss &
HANDLER=$!
slimium eval 'alert("example")'
wait "$HANDLER"
slimium permissions reset
```

Overrides set by these thin wrappers (geolocation, throttle, script, cache) last for the owning
connection. Use `batch` when several operations must share exactly the same connection and
overrides. The persistent emulation (viewport, orientation, touch, UA, offline, block) and dialog
handling belong to the session keeper instead (docs/EMULATION.md). A raw `cdp Emulation.*` call
for device metrics, touch or UA makes the keeper re-assert the session state afterwards.

## Downloads and browser history

Chromium 156 advertises download behavior/cancellation methods and download events, but no
retrospective `downloads list` query. It also has no browser-history query in the live CDP
schema. slimium therefore does not invent either capability or maintain a shadow history.
Use the generic methods/events for downloads, for example
`Browser.setDownloadBehavior` with `eventsEnabled:true` and `--events MS`, or configure the
existing `download-dir` command. Browser history is outside CDP; slimium does not read the
engine's private profile databases as a substitute.
