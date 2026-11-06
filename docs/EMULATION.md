# Emulation keeper and JavaScript dialogs

slimium commands are short-lived. DevTools drops a session's Emulation overrides when that session
detaches, so before this design slimium re-applied the saved viewport/UA at the start of every
command. Anything that loaded *between* commands therefore saw the real headless window. That
includes a page that reloads or navigates itself, an SPA route change followed by a reload, a new
tab's first load, and a popup. Measured on Chromium 156 headless=new: 780x493, landscape-primary,
maxTouchPoints 0, pointer:fine. Orientation and touch were never emulated at all.
A JavaScript dialog nobody handled (beforeunload armed after user activation) also wedged the
session. `nav` timed out, and every later command hung 30 s re-applying emulation.

## Design: one keeper per session

`slimium start` and `slimium attach` spawn a detached keeper: the same binary running the internal
command `slimium __hold --session S`. It is double-forked, gets its own session id, and has stdin
and stdout on /dev/null and stderr in `<state>/S.holdlog`. It is `CF_NOBATCH`, and MCP's generic
`slim_cli` cannot call it. If the keeper is missing, the first command that needs it starts one
lazily: an emulation command, or any command that attaches to a page.

The keeper holds **one** browser websocket:

- `Target.setAutoAttach {autoAttach:true, waitForDebuggerOnStart:true, flatten:true,
  filter:[{type:"page"}]}` on the browser target. This attaches to every existing page and to
  every new one: `Target.createTarget` (`open`), `window.open` and `target=_blank` popups. New
  pages are paused before their first navigation. The keeper sends `Page.enable` and the emulation
  calls, then `Runtime.runIfWaitingForDebugger`. The first load of a new tab already sees the
  emulated metrics (tested: `open` and a popup record 390x844 portrait dpr 3 touch on their first
  load).
- The overrides live in this persistent session. Reloads and navigations (by slimium or by the
  page itself) keep them.
- The loop is fully event driven. It polls the websocket plus a self-pipe written by its signal
  handler, and blocks for up to 15 s when idle. Every message is drained and freed immediately, so
  no queue grows without bound. Responses are matched to a small table of outstanding ids.
- It exits when the browser connection closes (browser killed or closed) or when the session
  state file disappears or points to another browser. `stop` also stops it first, with SIGTERM to
  the validated pid and then SIGKILL after 2 s. A flock on `S.holdlock` guarantees one keeper per
  session. A second `__hold` exits at once.
- Pid validation, done for every signal and status read, is exact: `/proc/PID/stat` start time
  equal to the recorded one, `/proc/PID/exe` a `slimium*` executable (a reinstalled binary shows
  as `(deleted)` and still matches), and argv exactly `slimium __hold --session S`.

Files in the session state directory (0700): `S.hold` (keeper pid, start time, acknowledged
generation, last request result, browser UA and version; 0600), `S.holdreq` (one request),
`S.pending` (open manual dialogs), `S.dialogs` (dialog log, 0600, trimmed to the newer half beyond
64 KB), `S.holdlock`, `S.holdlog`. When a keeper runs for a session started by slimium, it also
writes `<user-data-dir>/.slimium-keeper`; see "Governor" below.

### Ownership and generations

The emulation fields in `S.json` carry a generation `gen`. These commands write the session
state with a bumped generation, send SIGUSR1 to the validated keeper, and return only after the
keeper acknowledges that generation (bounded at 5 s):

- `viewport` / `resize`
- presets
- `ua`, `offline`, `block`
- `responsive` (each size and the restore)
- `dialog policy`

The keeper acknowledges after every page answered the calls, or after at most 3 s for a hung page.
It re-applies only when the resulting call plan changed: a policy-only change does not touch the
page.

Ordinary commands do **not** apply emulation while the keeper runs and has acknowledged the
current generation. That check costs one small file read plus /proc validation. A second session
setting or clearing overrides would conflict, and the per-command work is gone (see measurements).
State writes by commands (current tab, websocket URL) merge with the file on disk, so long-lived
pipe/MCP processes do not roll back a newer generation.

Disturbance, measured: a second session that only attaches, evaluates, screenshots, enables
Page/Runtime/Network and detaches leaves the overrides intact. Tested with `eval`, `shot`,
`snapshot`, `text`, `pipe`, an MCP `eval` + `screenshot`, and `responsive`. A second session
that *sets* device metrics and detaches resets the metrics for everyone (back to 780x493, DPR 1,
landscape). Re-sending identical params from the keeper then restores only width/height, not DPR
or orientation, so a re-assert must clear first. slimium avoids this:

- `responsive` goes through the keeper.
- The record worker skips its own metrics while a keeper owns the session.
- `shot` is captured by the keeper (below).
- `cdp Emulation.*DeviceMetrics*|*Touch*|*UserAgent*` detaches its own session and then asks the
  keeper to re-assert (clear + set).

Screenshots: only the session that set the device metrics gets a DPR-sized
`Page.captureScreenshot`. Another session gets 390x844 instead of 1170x2532. A `clip.scale`
capture from another session resets the keeper's metrics. In keeper mode, `shot` (and MCP
`screenshot`) therefore asks the keeper to capture into the file. The keeper decodes the image in
place and writes the file itself. The clip for `--full`/`--selector` is still computed by the
command.

### Emulation state

| field | meaning |
|---|---|
| `vp_w`, `vp_h` | CSS viewport; also `screenWidth`/`screenHeight` |
| `vp_scale` | deviceScaleFactor (0 = the real one) |
| `vp_mobile` | `mobile` (meta viewport, overlay scrollbars, text autosizing) |
| `vp_orient` | auto (portrait iff H >= W), portrait, landscape -> `screenOrientation` portraitPrimary/0 or landscapePrimary/90 |
| `vp_touch` | auto (= mobile), on, off -> `Emulation.setTouchEmulationEnabled {maxTouchPoints:5}`: maxTouchPoints 5, `(pointer: coarse)`, `(hover: none)`, `ontouchstart` |
| `ua` | explicit user agent (always wins) |
| `ua_preset` | `mobile`/`tablet`: Android UA derived from the engine |
| `offline`, `block` | Network emulation (the keeper enables Network only while one is set) |
| `dlg` | dialog policy |

**Touch input.** `Emulation.setEmitTouchEventsForMouse` is deliberately *not* used. With it,
`Input.dispatchMouseEvent mousePressed` never answers: measured 4.3 s timeout, touchstart without
touchend. While touch emulation is on, `click`/`dblclick` (and act/pipe clicks) send a real tap
instead (`Input.dispatchTouchEvent` touchStart + touchEnd). That yields pointerdown:touch,
touchstart, touchend, mousedown, mouseup and click:touch, as on a phone. A hidden tab (e.g. behind
a popup) never acknowledges touch input, so a tap first brings the tab to the front. `hover` and
`drag` stay mouse events.

**User agent.** Precedence: explicit `ua`, then the preset UA, then the default UA. With no
explicit UA:

- `viewport mobile` sends
  `Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko)
  Chrome/<major>.0.0.0 Mobile Safari/537.36`, with `<major>` taken from `Browser.getVersion`. The
  userAgentMetadata says platform Android 14.0.0, model Pixel 8, mobile:true, formFactors Mobile,
  and navigator.platform is `Linux armv8l`.
- `viewport tablet` uses the Android *tablet* form: no "Mobile" token, mobile:false, formFactors
  Tablet. Android tablets send exactly this, and sites then serve their tablet layout.
- With neither, `HeadlessChrome/` in the engine UA becomes `Chrome/`. The headless token changes
  site behaviour. `SLIM_KEEP_HEADLESS_UA=1` opts out.

An override without `userAgentMetadata` empties `navigator.userAgentData` (measured: brands [],
platform ""), which is itself a fingerprint. Derived UAs therefore always carry metadata. The
brands are Chromium plus Chromium's GREASE brand for that major version (`Not:A-Brand` "8" for
156, ordered as the engine does), and the full version comes from the engine. An explicit `ua`
string is sent as given, without metadata.

## Presets

| preset | size | dpr | mobile | touch | orientation | UA |
|---|---|---|---|---|---|---|
| `viewport mobile` | 390x844 | 3 | yes | yes | portrait | Android phone (Pixel 8) |
| `viewport tablet` | 768x1024 | 2 | yes | yes | portrait | Android tablet |
| `viewport desktop` | 1440x900 | 1 | no | no | landscape | default (HeadlessChrome -> Chrome) |

`viewport reset` clears metrics, touch and the preset UA. `viewport W H [--scale F] [--mobile]`
keeps its meaning. It now also emulates orientation (auto from W/H) and, with `--mobile`, touch.
`--touch`/`--no-touch` and `--orientation portrait|landscape|auto` override these.

## JavaScript dialogs

The keeper enables Page in every page session and handles `Page.javascriptDialogOpening` by policy:

| type | default |
|---|---|
| beforeunload | accept (the navigation was intended) |
| alert | accept |
| confirm | dismiss |
| prompt | dismiss |

~~~sh
slimium dialog policy                               # show
slimium dialog policy confirm=accept,prompt=manual  # TYPE=accept|dismiss|manual, TYPE=all|alert|confirm|prompt|beforeunload
slimium dialog policy reset
slimium dialogs [--last N] [--clear]                # log: time, type, message (200 chars), url, action, tab
slimium dialog accept [TEXT] | dismiss               # answer a pending (manual) dialog
slimium dialog auto accept|dismiss [--timeout MS] [--count N]
~~~

- A `manual` dialog stays open and is recorded in `S.pending` and in the log. Only the session
  that had Page enabled when the dialog opened can answer it. A fresh session gets "No dialog is
  showing", and its `Page.enable` blocks until the dialog closes; that is why
  `cdp Page.handleJavaScriptDialog` could not unwedge the session. `dialog accept|dismiss`
  therefore asks the keeper.
- Commands fail fast while a manual dialog blocks their tab. `ctx_page` checks `S.pending`, and
  a CDP wait hook re-checks it every 100 ms, which also covers `eval 'confirm(1)'` in flight. The
  error names the dialog, e.g. `a JavaScript confirm dialog "M1" is open on this tab (dialog
  policy: manual); run 'slimium dialog accept|dismiss'`. Measured: under 1 s.
- `dialog auto` lends the keeper a temporary `all=accept|dismiss` policy, counts the handled
  dialogs in the log, and restores the previous policy.
- Without a keeper (`SLIM_HOLD=0`, stateless `--port`), `nav`, `reload`, `back`, `forward`
  and `close` enable Page in their own session and answer dialogs by policy while they wait. A
  beforeunload is accepted, so the navigation/close completes in about 0.5 s instead of timing out.
- Re-applying emulation without a keeper uses 3 s bounded waits.

## Modes

| mode | emulation | dialogs |
|---|---|---|
| session (default) | keeper; persistent across loads/tabs | keeper policy |
| `SLIM_HOLD=0` | old behaviour: re-applied by every command, lost between commands | nav/reload/back/forward/close accept beforeunload themselves |
| `--port N` (stateless) | only within the command | as above |
| `SLIM_NO_PROC=1` | as `SLIM_HOLD=0` (no /proc to validate a keeper) | as above |

## Governor

The keeper keeps every tab attached, so the governor's opt-in hibernation can no longer read
"attached" as "a client uses this tab". The keeper writes `<udd>/.slimium-keeper` (pid + start
time). While that file names a live process, the governor hibernates tabs by age/recency alone.
It lists hibernated tabs in `<udd>/.slimium-hibernated`. The next command for such a tab restores
it itself (`Page.navigateToHistoryEntry`, waits for load), and the governor logs
`restore ... "by":"client"`.

## Measurements (Chromium 156, this machine)

- **Keeper:** VmRSS 2.2-2.5 MB. CPU 0 ticks over 5 s idle: it blocks in poll and has no timers
  except a 15 s state check.
- **Command latency**, `eval 1` on the same fixture, average of 30 runs:

  | case | latency |
  |---|---|
  | main, emulation set (re-applied per command) | 1.77 ms |
  | main, no emulation | 1.34 ms |
  | keeper (emulation set) | **1.26 ms** |
  | `SLIM_HOLD=0` fallback (also re-applies the UA fix) | 2.27 ms |

- **Command RSS:** unchanged, 1.9-2.2 MB.
- `viewport` with acknowledgement: a few ms. `nav` past an armed beforeunload: ~0.5 s with the
  keeper or the fallback (previously an 8-30 s timeout and a wedged session).

## Limitations

- Overrides are per page target. Dedicated/shared/service workers still report the engine's UA.
- A dialog that was already open before the keeper attached (e.g. from an old slimium) cannot be
  answered: Chromium does not report it to a new session, and `Page.enable` blocks until it
  closes.
- A raw `cdp` call that sets device metrics from another session resets the keeper's metrics until
  the keeper re-asserts them. slimium's own `cdp` command does that automatically.
- Two sessions attached to one browser (e.g. `attach` to a browser another session started) have
  two keepers. Their emulation and dialog policies would compete.
- Live orientation changes reach a hidden tab only when it becomes visible again (`switch`).

## Tests

`tests/emulation.sh` (in `make test-modules`) uses only CDP 9572-9575 and fixture HTTP 9588 with
fresh /tmp state, profiles and data. It covers: self reload, `reload`, client route + page-initiated
navigation, new tab and popup first loads, landscape, forced orientation, `--no-touch`, reset,
presets with UA/userAgentData, and an explicit UA winning. A story-editor fixture uploads a file,
pushes `/story/editor`, reloads itself, and must not show "Rotate your device" in portrait (but
does in landscape). The screenshot must be exactly 1170x2532 with exact canvas2D and WebGL pixels.
The suite also covers non-disturbance by eval/shot/snapshot/pipe/MCP/responsive/raw cdp; the
beforeunload/alert/confirm/prompt policies; manual dialogs failing fast and `dialog accept [TEXT]`;
and keeper lifecycle (one per session, lazy respawn, exits with the browser, `stop` leaves
nothing, governor, `SLIM_HOLD=0`, `--port`, `attach`). No command may take 10 s.
