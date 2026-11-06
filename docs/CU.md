# cu on slimium: `cu-slim`

`cu-slim` is a second, separately deployed [cu](https://github.com/jaivial/cu)
whose browser is slimium (`slimium-chrome`). It runs next to the user's own cu
and never touches it.

| | user's cu | cu-slim |
|---|---|---|
| binary | `~/.cargo/bin/cu` (via `~/.local/bin/cu`, `~/bin/cu`) | `~/.local/bin/cu-slim-bin` via `~/.local/bin/cu-slim` |
| daemon | systemd --user `cu-daemon.service`, `cu daemon` | `cu-slim daemon` (or auto-started by the first CLI call) |
| data dir | `~/.cu/daemon` | `~/.local/share/cu-slim` (`CU_DAEMON_DIR`) |
| HTTP | 127.0.0.1:**8810** (cu's `DAEMON_PORT`, walks up if busy) | 127.0.0.1:**8790** |
| DevTools | 127.0.0.1:**9362** (`DAEMON_CDP_BASE`, walks up to 9386) | 127.0.0.1:**9390** |
| browser | `chromium` (Fedora script) | `~/.local/bin/slimium-chrome` if installed, else the stand-in |

Note: the canonical cu daemon is on 8810/9362, not 8787/9222 (those are only
the defaults of the legacy `cu start`). 9362-9386 must stay free for it: no
slimium test may bind there.

## Install

```sh
# 1. cu with the small changes cu-slim needs (branch slimium-integration of the cu repo)
cd ~/cu && git worktree add ~/cu-wt/slimium slimium-integration   # already done here
cd ~/cu-wt/slimium && CARGO_TARGET_DIR=~/cu-wt/slimium/target cargo build --release
# 2. binary + launcher + stand-in
~/slimium-wt/cu-integration/tests/cu/install.sh
```

`install.sh` writes only `~/.local/bin/cu-slim`, `~/.local/bin/cu-slim-bin` and
`~/.local/share/cu-slim/bin/slimium-chrome-standin.sh`. When the real wrapper is
installed as `~/.local/bin/slimium-chrome`, the launcher picks it up
automatically (no reinstall).

## Use

Exactly like cu, with `cu-slim` instead of `cu`:

```sh
cu-slim daemon &                     # or let the first call start it
cu-slim navigate https://example.com
cu-slim snapshot
cu-slim click e1 · cu-slim type e2 "text" --submit · cu-slim act '[...]'
cu-slim shot page.png · cu-slim text · cu-slim tabs · cu-slim --new-tab navigate URL
cu-slim status · cu-slim diagnostics
```

Contexts (isolated cookie jars) and leased tabs have no CLI in cu 1.9.0 (the
README's `cu tab open --lease`, `cu context open`, `cu lease`, `cu batch` are
not implemented); use the HTTP API with the token from
`~/.local/share/cu-slim/server.json`: `POST /v1/navigate?context=NAME`,
`GET /v1/contexts`, `DELETE /v1/contexts/NAME`, `POST /v1/tabs {"url","owner_pid","fresh":true}`.

Stop it: `kill $(pgrep -f '^/home/[^ ]*/.local/bin/cu-slim-bin daemon')` (SIGTERM; the
daemon closes its browser). Never `pkill cu`: that would hit the user's cu.

### Environment of the launcher

| variable | default | meaning |
|---|---|---|
| `CU_SLIM_HOME` | `~/.local/share/cu-slim` | daemon data dir (exported as `CU_DAEMON_DIR`) |
| `CU_SLIM_PORT` | 8790 | HTTP port (`CU_PORT`) |
| `CU_SLIM_CDP_PORT` | 9390 | DevTools port (`CU_CDP_PORT`) |
| `CU_SLIM_BROWSER` | slimium-chrome / stand-in | browser executable (`CU_BROWSER`) |
| `CU_SLIM_MODE` | `fast` | cu launch profile (`CU_MODE`) |
| `SLIM_PROFILE` | `balanced` | slimium memory profile, passed through to the browser (off/balanced/max) |

The launcher always sets `CU_HEADLESS=1` and unsets `CU_DATA_DIR` (which would
put the cu CLI in its legacy single-tab mode, without per-agent sessions),
`CU_VAULT_HOST`, `CU_PUBLIC_URL`, `CU_MIN_DEFAULT`, `CU_DISPLAY`.

## Changes to cu (branch `slimium-integration`, worktree `~/cu-wt/slimium`)

`cargo fmt --check` and `cargo test` pass (169 tests). Not pushed; not merged into cu's main.

1. `7fc156e` **`CU_PORT`** sets the HTTP port of `cu start` and `cu daemon`;
   `CU_CDP_PORT` now applies to `cu daemon` too (it only applied to `cu start`);
   an auto-started `CU_DAEMON_DIR` daemon no longer asks systemd for the
   canonical `cu` unit. Without this a second daemon's ports were whatever
   was free after 8810/9362.
2. `a857f14` **fix:** `cu mark`, `cu console` and `cu upload` work in the
   session's tab. Under the daemon they went to the browser's default tab
   (`mark`/`console` used `request` instead of `page_request`; `/v1/upload`
   was missing from the routes that resolve the session's tab), so they failed
   with "this page has no refs yet" or showed nothing. Not slimium-specific.

cu needs nothing slimium-specific: it detects the browser kind only by
executable name (`headless_shell`/`headless-shell` → no `--headless=new`;
`min` → Min/Electron), so `slimium-chrome` is treated as a full Chromium and
gets `--headless=new` plus a `--user-agent` built from `slimium-chrome --version`.
The browser inherits the daemon's environment, so `SLIM_PROFILE` reaches it.

## Tests: `tests/cu/run.sh`

```sh
tests/cu/run.sh                     # 71 checks, ~60 s
PARALLEL=50 tests/cu/run.sh         # more parallel agents
CU_SLIM_TEST_BROWSER=/usr/lib64/chromium-browser/chromium-browser tests/cu/run.sh   # any browser
SLIMIUM_CHROME=~/slimium-wt/core/slimium-chrome tests/cu/run.sh                     # a wrapper build
CU_BENCH=1 tests/cu/run.sh          # also cu's own bench/parallel_tabs.py (informational, see below)
```

It starts its own cu-slim daemon (temp data dir, HTTP 8792, CDP 9392), serves
`tests/cu/fixtures` on 127.0.0.1:8791, and checks through the `cu-slim` CLI:
status, diagnostics (the browser really is slimium-chrome), isolation
(server.json port, CDP port); navigate, snapshot, type by ref, `act` batch
(type/select/check/click), press; links and click-navigation; popup → `tabs`,
`--tab`, `tabs close`; iframe click and type; shadow DOM click, `inspect`
(`>>>` selector), `mark`/`marks`/`marks --md`/`marks clear`; long page scroll +
click; `shot` PNG (magic + size), `--selector`, `--annotate all`, JPEG;
`eval`; `console --level error`; `act` wait; login with cookie and no echo of
the password; contexts (`Target.createBrowserContext`): isolated jars, own
login, default untouched, list, close; `session save/load`; download (file
lands in the daemon's dir, listed by `downloads`); `upload`; leased tab over
`POST /v1/tabs`; `--new-tab`; `targets`; **20 parallel agents** (each its own
process → its own leased tab: open → navigate → snapshot → act by ref → judge
text → close), all distinct tabs, all closed afterwards. Before and after it
fingerprints the user's cu (daemon PIDs, its browser PIDs, listeners on
8810-8834 / 9362-9386) and fails if anything changed. A trap stops only the
PIDs it started.

Script authors: cu's session is the nearest ancestor that is not a one-shot
`sh -c`, and every `$(...)` is its own subshell, so `x=$(cu snapshot)` in a
script is a *new session and a new tab* each time. run.sh calls cu in the
current shell and reads the output from a file (`cux VAR args`).

Results (2026-11-05, Chromium 156.0.8078.79):

| browser | result |
|---|---|
| slimium stand-in, balanced / max | 71/71 |
| core's `slimium-chrome` (`~/slimium-wt/core`, uncommitted build) | 71/71 |
| stock ELF `/usr/lib64/chromium-browser/chromium-browser` | 71/71 |
| `chrome-headless-shell` 156.0.8078.4 (Playwright 1248) | 71/71 |

Two checks are reported as **KNOWN** (cu issues, identical on stock Chromium,
not counted as failures): `cu text` stops at 16 000 characters but answers
`"truncated":false`; a click that starts a download does not answer
`"download":"name"` (cu waits for `Page.downloadWillBegin`, which Chromium 156
does not send there) although the file is downloaded and listed. cu's own `bench/parallel_tabs.py` is stale against
cu 1.9.0 (it passes `"tab"` in the JSON body, which the daemon ignores, so its
flows share one tab): 5 flows → 3 ok / 2 × 502, 20 → 6 / 14, the same on the
stand-in and on stock Chromium. cu's `cargo test` uses a fake Chromium and
cannot target a real browser. Also seen: an
element shot (`--selector`) on a 400-paragraph page takes 3-6 s on any
Chromium and can hit cu's 5 s DevTools timeout ("os error 11").

## RAM through cu: `tests/cu/ram.sh`

Flow per run: fresh profile; 10 leased tabs (`cu-slim --new-tab navigate`) on
the fixtures + example.com, en.wikipedia.org/wiki/Web_browser,
news.ycombinator.com; `snapshot --tab` in each (13/13 every run); settle 5 s;
PSS summed over the browser process tree (`/proc/<pid>/smaps_rollup` of the
daemon's browser child and all descendants; median of 3 samples). Median of 3
runs. All configs: same cu-slim binary, `CU_HEADLESS=1`.

| config | PSS median | procs | vs stock-elf (cu fast) | vs stock-compat |
|---|---:|---:|---:|---:|
| `slim`: stand-in, balanced (caller-wins merge) | 365.3 MB | 11 | 0.985 | 0.693 |
| `slim-max`: stand-in, max | 349.4 MB | 9 | 0.942 | 0.663 |
| stand-in, balanced, profile's `--renderer-process-limit=1` overrides cu's 4, + `NetworkServiceInProcess2` | 313.2 MB | 8 | 0.845 | 0.594 |
| same + `--disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup` + `--disable-component-extensions-with-background-pages` | **275.6 MB** | 7 | **0.743** | **0.523** |
| same + `--in-process-gpu` | 286.6 MB | 6 | 0.773 | 0.544 |
| `stock-script`: `/usr/bin/chromium-browser` (Fedora script, how cu runs it by default), cu fast | 375.2 MB | 13 | | |
| `stock-elf`: Chromium ELF, cu fast flags only | 370.8 MB | 11 | 1 | |
| `stock-compat`: Chromium ELF, `CU_MODE=compat` (cu adds 3 flags: plain Chromium) | 527.3 MB | 25 | | 1 |

Raw: `tests/cu/results/ram-final.txt`, `ram-final-limit1-nsip2.txt`, `ram-final-webui.txt`, `ram-final-webui-ipgpu.txt`.
Single-run check of core's real wrapper (uncommitted build): balanced 356 MB
(9 procs), max 322 MB (7 procs).

**The ≤ 50 % target is not met through cu.** cu's fast mode already passes most
of what the profile does (`--disable-gpu`, `--disable-extensions`,
`--disable-site-isolation-trials`, background services off,
`--renderer-process-limit=4`), so the stock baseline cu really runs is already
lean (371 MB vs 527 MB for plain Chromium). Per-process PSS with limit 1:
browser 114 MB, renderers 88 + 43 MB, GPU process 58 MB (exists despite
`--disable-gpu`: headless SwiftShader), NetworkService 32 MB, three zygotes
25 MB, storage 8 MB: ~237 MB is fixed overhead outside the renderers, above the
~185 MB that 50 % allows. Critical memory pressure + GC over CDP saved ~7 %.

## What the flag profile must and must not do for cu

Measured with `tests/cu/variants.sh` (full suite + one RAM run per variant;
`tests/cu/results/variants.txt`):

| variant (on top of cu fast mode) | suite | PSS | procs |
|---|---|---:|---:|
| balanced as is | 71/71 | 368 MB | 11 |
| `--renderer-process-limit=1` overriding cu's 4 | 71/71 | 342 MB | 9 |
| + `--in-process-gpu` | 71/71 | 351 MB | 8 |
| + `--enable-features=NetworkServiceInProcess2` | 71/71 | 316 MB | 8 |
| + `--no-zygote --no-sandbox` | 71/71 | 387 MB | 6 |
| + `--no-sandbox` | 71/71 | 347 MB | 8 |
| + `--js-flags=--lite-mode` | 71/71 | 340 MB | 9 |
| + `--js-flags=--jitless` | 71/71 | 342 MB | 9 |
| + `--single-process` | **53/71** | 280 MB | 4 |
| + in-process-gpu, NSIP2, no-zygote, no-sandbox, lite-mode | 71/71 | 360 MB | 4 |
| + NSIP2 + no WebUI omnibox popups + no component extensions | 71/71 | 299 MB | 7 |
| + the same + `--in-process-gpu` | 71/71 | 290 MB | 6 |
| chrome-headless-shell 156, stock | 71/71 | 537 MB | 10 |
| headless-shell + limit 1 + the combo above | 71/71 | 492 MB | 2 |

- **`--single-process` breaks cu.** `Target.createBrowserContext` → cu answers
  "Chromium did not open a tab in the new context" and the browser then dies
  (DevTools connection refused), so contexts, downloads, upload, leased tabs,
  `--new-tab` and the parallel flows all fail. Never use it under cu.
- **`--no-zygote` costs memory** (+45 MB: forked renderers no longer share
  the zygote's pages). Avoid.
- **Chromium honours only the last `--enable-features` / `--disable-features`.**
  cu passes its own (`NetworkServiceInProcess`; `Translate,...,LensOverlay,
  PaintHolding,SpareRendererForSitePerProcess,BackForwardCache`). The merge
  must union them into one switch each; an appended second
  `--disable-features` silently drops cu's list. (Core's current build appends
  a second one; the stand-in merges.)
- **cu passes `--renderer-process-limit=4`**: with caller-wins merging the
  profile's 1 is lost. Memory switches must override the caller.
- headless=new 156 preloads WebUI renderers (omnibox popups) and a component
  extension: `--disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup` and
  `--disable-component-extensions-with-background-pages` remove them (-38 MB
  through cu, suite passes). `--in-process-gpu` did not help in the 3-run median.
- cu's `--enable-features=NetworkServiceInProcess` has no effect on Chromium
  156; `NetworkServiceInProcess2` does (-30 MB, suite passes).
- `slimium-chrome --version` must print `Chromium 156...` on stdout: cu builds
  its `--user-agent` from it.
- Keep: `--remote-debugging-port`, `--user-data-dir=`, `--headless=new`,
  `--user-agent=`, `--lang`, `--window-size`, `--screen-info`, the start URL
  (cu's argv, see the user's daemon's command line); `Target.createTarget`,
  `Target.createBrowserContext`, `Browser.setDownloadBehavior` with
  `eventsEnabled`, `Page.windowOpen` must keep working.
- Do not use `Memory.forciblyPurgeJavaScriptMemory` on cu's tabs (it destroys
  the V8 contexts cu evaluates in). `HeapProfiler.collectGarbage` and
  `Memory.simulatePressureNotification` are safe.

## Files

- `tests/cu/cu-slim` launcher (installed as `~/.local/bin/cu-slim`)
- `tests/cu/install.sh` installer
- `tests/cu/slimium-chrome-standin.sh` stand-in wrapper (feature-list merge; knobs `SLIM_ENGINE`, `SLIM_ENGINE_DIRECT`, `SLIM_EXTRA`, `SLIM_OVERRIDE`, `SLIM_STANDIN_LOG`)
- `tests/cu/lib.sh` helpers (user-cu fingerprint, process-tree PSS, instance start/stop)
- `tests/cu/run.sh` feature suite, `tests/cu/ram.sh` RAM comparison, `tests/cu/variants.sh` flag matrix
- `tests/cu/fixtures/` pages (form, login/account, links/page2, iframe/frame, shadow, long, download, late, upload)
- `tests/cu/results/` measured output

## MCP: direct slimium versus native cu MCP

Two different MCP servers are available; they do not replace one another:

| | `slimium mcp` | `cu-slim mcp` |
|---|---|---|
| implementation | slimium's C11 in-process `cli_invoke` dispatcher | cu's own Rust `src/mcp.rs`, translating calls to its daemon's HTTP routes |
| browser/session | one selected slimium session and one reused CDP connection | one cu daemon; each MCP process gets its own leased page/session |
| best for | browser lifecycle, CLI-compatible commands, raw CDP, eval, screenshots to files, minimal adapter overhead | compact ref-addressable snapshots/actions, contexts, vault/session save, uploads/downloads, console, QA marks, human handoff |
| tab lifetime | slimium's current tab persists across MCP connections; browser stays up at MCP EOF | tabs opened by the MCP session are closed at clean disconnect; the daemon is the backstop after a kill |
| logging | no argument/result logging; optional `SLIM_MCP_LOG` | cu logs no tool arguments; its protocol log goes to stderr |

**cu does not internally use slimium's MCP server.** `cu-slim mcp` is cu 1.9.0's native MCP
implementation. Its browser is slimium because the `cu-slim` launcher sets
`CU_BROWSER=~/.local/bin/slimium-chrome` (resolving to `~/.local/bin/slimium`), not because cu
loads or invokes `slimium mcp`. Use direct slimium MCP when low-level CLI/CDP access is wanted;
use cu MCP when cu's higher-level refs and daemon services are wanted. They can coexist, but
should not drive the same tab concurrently unless that coordination is intentional.

Exact isolated client configuration (only HTTP 8790 / CDP 9390 and
`~/.local/share/cu-slim` are used by `cu-slim`):

```json
{
  "mcpServers": {
    "slimium": {
      "command": "/home/jaime/.local/bin/slimium",
      "args": ["--session", "default", "mcp"]
    },
    "cu-slim": {
      "command": "/home/jaime/.local/bin/cu-slim",
      "args": ["mcp"],
      "env": {
        "CU_SLIM_HOME": "/home/jaime/.local/share/cu-slim",
        "CU_SLIM_PORT": "8790",
        "CU_SLIM_CDP_PORT": "9390"
      }
    }
  }
}
```

The equivalent client CLI registrations are:

```sh
claude mcp add slimium -- slimium --session default mcp
claude mcp add cu-slim -- cu-slim mcp
codex mcp add slimium -- slimium --session default mcp
codex mcp add cu-slim -- cu-slim mcp
```

See [MCP.md](MCP.md) for direct slimium schemas, framing, safety and benchmarks.

### Native cu MCP verification

```sh
tests/cu/mcp-integration.sh
```

This test uses the already deployed `~/.local/bin/cu-slim` and does not rebuild or install
anything. It verifies that diagnostics resolve the browser to the real `slimium` binary on
8790/9390, then runs two native cu MCP clients: initialize/list/status, two distinct data-URL
pages, same-connection `text`/`snapshot`/`tabs`, and clean shutdown. Current result: **30 checks,
0 failures**, in both already-running and cold-start modes. If 8790 is closed, the script starts
the isolated daemon itself and records its PID; its trap stops only that PID. An already-running
isolated daemon is left running. It never accesses the user's real cu, its ports, data, or
processes. No `make install` was needed for this verification; the orchestrator owns final
installation of the merged slimium binary.
