# Interaction: locators, stable refs and streaming pipelines

The interaction module adds bounded page snapshots, universal locators and two low-overhead
execution interfaces. It is registered by `slim_interact_commands(size_t *)`; no extra process,
library or browser is required.

## Locators

All existing commands that take a selector (`click`, `dblclick`, `fill`, `type`, `hover`,
`select`, `check`, `wait`, `scroll-to`, `text`, `html` and selector screenshots) accept
these forms:

| form | meaning |
|---|---|
| `button.primary`, `css=button.primary` | CSS (legacy behavior preserved) |
| `//button`, `xpath=//button` | XPath |
| `text=Save` | normalized, case-insensitive text substring; deepest match wins |
| `text="Save"` or `text='Save'` | exact normalized text |
| `role=button` | explicit or common implicit ARIA role |
| `role=button[name="Save"]` | role plus exact accessible name |
| `testid=save` | `data-testid`, `data-test-id` or `data-test` |
| `label=User name` | associated label, aria-label or placeholder (quoted = exact) |
| `@e7` | stable element reference from `snapshot` or `find` |

Text, role, testid and label searches include open shadow roots. `find` searches every frame
listed by CDP, including cross-origin frames. Ordinary CSS/XPath actions retain their legacy
main-frame behavior; use a frame ref returned by `find` to act in another frame.

## Compact snapshots

```sh
slimium snapshot [--interactive] [--all] [--selector S] [--max N]
slimium find LOCATOR [--max N]
slimium inspect LOCATOR
```

`snapshot` emits one compact line per element (or an `elements` array with `--json`):
ref, role/tag, accessible name, input type/state, visibility, integer box and frame path. The
default includes controls and useful semantic elements; `--interactive` keeps actionable
controls and `--all` walks every element. `--selector` scopes the walk. The default maximum is
200 and the hard maximum is 2000. Names, text and values are capped (120/160/120 bytes as
applicable) and truncation is explicit.

Frame `0` is the main document; `0/1` is its second child frame. Open shadow roots are
walked in place. Cross-origin frames are evaluated in their own CDP execution context, without
relaxing origin checks. Coordinate-based iframe actions translate owner-frame boxes to the top
viewport before dispatching input.

Password input values are never returned. Snapshots show `password:true` in JSON and
`value=<redacted>` in text; `inspect` also omits the password value attribute. Other input
values are bounded and may be shown, so do not put secrets in ordinary text inputs.

### Ref lifetime and state

Refs remain stable across separate CLI invocations and repeated snapshots in the same document.
They are per session/port, target and document. Metadata is stored atomically as
`<session>.interact.json` (mode 0600) in the slimium runtime directory (mode 0700). It contains
only ref numbers, frame/document identities and frame paths—not element values or page text.
At most 4096 refs are retained.

A full or same-document navigation, a changed document, a detached element or use in the wrong
tab produces a stale/unknown-ref error. slimium never silently clicks a replacement. Run
`snapshot` or `find` again after navigation or substantial DOM replacement.

`find` returns count and refs. `inspect` requires exactly one match and returns a unique CSS
selector, safe attributes, box and a small computed-style set.

## Direct actions

```sh
slimium upload LOCATOR FILE...
slimium drag FROM TO
slimium focus LOCATOR
slimium submit LOCATOR
slimium form fill JSON|@file
```

`upload` uses `DOM.setFileInputFiles` and requires readable regular files. `drag` dispatches
real mouse events with intermediate motion. `submit` uses `requestSubmit` when available.
`form fill` accepts an object mapping locators to strings/numbers/booleans (or
`{"fields": {...}}`): booleans map to `check`, selects to `select`, other controls to
`fill`. Field results never echo submitted values.

## Action arrays

```sh
slimium act JSON|@file|- [--stop-on-error] [--snapshot]
```

Input is an array. Each item accepts either `{"action":"click","locator":"@e1"}` or
`{"action":"click","args":["@e1"]}`. Supported convenient fields include `value`/`text`,
`key`, `url`, `js`, `off`, `visible`, `gone`, and snapshot options. `cmd` is an alias
for `action`; `selector`/`target`/`ref` alias `locator`.

All actions use one browser connection and current tab. Ready actions run immediately; failed
click/fill/type/select/check actions get one bounded visibility wait and retry. The single JSON
result has `actions`, `failures` and `results`; every result has `index`, `action`, `ok`
and command fields or `error`. Per-action failures do not make the transport itself fail;
`--stop-on-error` stops after the first. `--snapshot` appends a final compact snapshot.
Nested `act`, `pipe`, `batch` and `mcp` are rejected.

## NDJSON pipe

```sh
slimium pipe [--stop-on-error]
{"id":1,"cmd":"snapshot","args":["--interactive"]}
{"id":2,"cmd":"click","args":["@e1"]}
{"id":3,"argv":["fill","testid=username","Ada","--timeout","1000"]}
```

One request and one compact, immediately flushed JSON response per line. `id` is optional and
is echoed verbatim. Every response has `ok`; command JSON fields are inlined, or `error` is
returned. Malformed lines are per-line errors and do not kill the stream. `--tab ID` and
`--timeout MS` may appear in request args; tab overrides are temporary and the original tab is
reattached afterward. `--session` and `--port` are fixed for the life of a pipe. Recursive
pipe/batch/MCP requests are rejected. EOF exits successfully, even if individual requests failed.

The same `ctx_t`, CDP websocket, attached target and Runtime/domain state are reused. Main-frame
refs need no target-list or backend-node round trip; frame context and owner-node lookups are only
used for iframe work and remain document-scoped. Act/pipe clicks pipeline their ordered mouse
events; ordinary main-frame fills pipeline insertion with the change/readback evaluation and
verify the resulting value, falling back to synchronous insertion if a readback races. Actionability
checks and error responses are retained.

## Testing and benchmark

```sh
tests/interaction.sh                    # CDP 9470, fixture HTTP 9488, fresh /tmp profile/state
bench/interaction.sh                    # isolated baseline/current browsers on 9471/9470
```

The test suite covers forms, every locator kind, delayed and hidden elements, open shadow DOM,
same/cross-origin frames, stale refs (full/hash navigation and detach), password non-disclosure,
act errors, malformed NDJSON, 100 streaming requests and PNG viewport dimensions. The benchmark
uses 100-operation workloads, reports median per-operation latency and throughput for separate
CLI, batch, act and pipe, and checks every response.

## Limits

Accessible names/roles are a compact DOM-based approximation, not a full browser accessibility
tree export. Closed shadow roots are not opened. Frames must still exist and have a CDP execution
context. Browser anti-bot dialogs, native file choosers and OS-level drag operations are outside
CDP's page interaction model.
