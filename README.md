# slimium

A Chromium-based browser for AI agents, written in C: the Chromium engine under a low-memory
process model, with every operation available from the command line, built to be cu's browser.

- **Half the RAM or less.** Same Chromium 156 engine, low-memory process model ([docs/PROFILE.md](docs/PROFILE.md)) plus a
  tiny memory governor ([docs/GOVERNOR.md](docs/GOVERNOR.md)): idle GC + memory-pressure and cgroup proactive reclaim of
  the engine's file pages (private reflinked engine copy, own systemd scope, pid preserved).
- **Pixel-identical rendering.** Screenshots match stock Chromium (0.0% pixel diff on text/web fonts, images, canvas,
  SVG, CSS, WebGL, full page, Wikipedia).
- **Everything on the CLI.** One <300 KB C binary (libc only), ~2 MB RSS on ordinary commands; all 674 methods
  advertised by Chromium 156 are invocable, with live schema discovery ([docs/CDP.md](docs/CDP.md)).
- **Fast element workflows.** Compact snapshots with stable `@eN` refs, CSS/XPath/text/role/test-id/label locators,
  `act` action arrays and a persistent NDJSON `pipe` ([docs/INTERACTION.md](docs/INTERACTION.md)); act/pipe cut
  median action latency by 88.2%/87.4% versus batch ([docs/PERFORMANCE.md](docs/PERFORMANCE.md)).
- **Persistent user profiles.** Named, isolated browser profiles keep website login storage across browser/daemon
  restarts; `profile save/restore` also restores tabs and scroll ([docs/PROFILES.md](docs/PROFILES.md)).
- **MCP without process churn.** `slimium mcp` offers 24 closed-schema tools over one persistent CDP connection,
  plus generic CLI/batch/CDP forwarding ([docs/MCP.md](docs/MCP.md)).
- **Screenshots, video and responsive testing.** Capture PNG/JPEG/WebP, record MP4/WebM/AVI, resize the live page
  without reloading and capture several breakpoints ([docs/MEDIA.md](docs/MEDIA.md)).
- **Built for cu.** `CU_BROWSER=slimium-chrome` is a drop-in browser for cu: cu's 71-check suite passes
  ([docs/CU.md](docs/CU.md)); `cu-slim` is a second, isolated cu deployment wired to slimium.

## Results (isolated footprint = Pss_Anon + Pss_Shmem + union of resident file pages, MB)

| workload | stock | slimium | ratio |
| --- | ---: | ---: | ---: |
| through cu, 13 tabs, steady median — vs plain Chromium (cu compat mode) | 709 | 225 | **0.32x** |
| through cu, 13 tabs, steady median — vs cu's own fast-mode Chromium | 532 | 225 | **0.42x** |
| through cu, steady time-weighted mean (vs cu fast) | 540 | 233 | 0.43x |
| through cu, peak during an active pass (vs cu fast) | 590 | 368 | 0.62x |
| standalone, workload A, steady median (`slimium start` vs stock ELF) | 828-836 | 410 | 0.49x |
| 56 tabs, flags only (no governor) | 2191 / 73 procs | 1155-1285 / 10 procs | ~0.55x |

Profile `balanced` (default). `max` adds V8 lite mode and PartitionAlloc hardening off (215 MB through cu, 0.40x).
Numbers and method: [docs/BENCHMARKS.md](docs/BENCHMARKS.md), [docs/GOVERNOR.md](docs/GOVERNOR.md), [docs/CU.md](docs/CU.md).
Trade-offs: the first action after a reclaim pays +30-110 ms once; private engine copies (reflinks on btrfs);
a single renderer process (no site isolation) — use it for agent work, not as a hardened daily browser.

See AGENTS.md for the rules and layout.

## Build

```sh
make                 # ./slimium (+ ./slimium-chrome symlink); C11, libc only, gcc -Os, stripped <300 KB
make test            # unit, functional, governor, profiles, interaction, media, protocol and MCP suites
make test-cu         # isolated cu-slim CLI + native MCP integration (ports 8790/9390)
make test-asan       # same under AddressSanitizer/UBSan/LeakSanitizer (host libasan or Flatpak SDK gcc)
make install         # PREFIX=~/.local -> ~/.local/bin/slimium + slimium-chrome symlink
```

## Usage

```sh
slimium start                           # headless browser, session "default", port 9230+
slimium nav example.com                 # scheme optional; ./file.html -> file://
slimium click 'a[href="/about"]'        # real mouse event at the element's center
slimium fill '#q' 'hello' && slimium press Enter
slimium text                            # document.body.innerText
slimium shot page.png --full            # png|jpeg|webp from extension or --format
slimium --json eval 'document.title'    # {"ok":true,"value":"...","type":"string"}
slimium snapshot --interactive          # compact elements with stable @eN refs
slimium click @e3
slimium fill 'label=Email' 'ada@example.test'
printf '%s\n' '{"cmd":"eval","args":["document.title"]}' '{"cmd":"click","args":["@e3"]}' | slimium pipe
slimium viewport 390 844                # live responsive resize, no reload
slimium record start run.mp4 --fps 10 && slimium record stop
slimium profile create work && slimium start --user work
slimium mcp                             # MCP stdio server for agent clients
slimium stop
```

There is no long-lived slimium process: each command connects to the browser's DevTools
websocket, attaches to the current tab, runs, and exits (~5-30 ms per command, ~2 MB RSS).
Session state lives in `$XDG_RUNTIME_DIR/slimium/<session>.json` (fallback `/tmp/slimium-$UID`,
override `SLIM_STATE_DIR`); the browser log next to it in `<session>.log`.

### Global options (anywhere before `--`)

| option | meaning |
|---|---|
| `--session NAME` | session to use (default `$SLIM_SESSION` or `default`) |
| `--port N` | talk to any CDP browser on 127.0.0.1:N without a session (stateless) |
| `--tab ID` | target tab (id or unique prefix) instead of the session's current tab |
| `--json` | one compact JSON object per line: `{"ok":true,...}` / `{"ok":false,"error":"..."}` |
| `--timeout MS` | per-operation timeout (default 30000); for `console` the collection time |

Exit codes: 0 ok, 1 error (one line on stderr), 2 usage error. `SLIM_DEBUG=1` traces DevTools traffic.

### Commands

| command | |
|---|---|
| `start [--profile off\|balanced\|max] [--headful] [--user-data-dir D] [--engine PATH] [--window WxH] [--no-gov] [-- ARGS]` | launch detached (setsid, log file); default headless, port 9230-9361 (skips 9222, 9362-9386), data dir `~/.local/share/slimium/<session>`; with the memory governor, own systemd scope and private engine copy unless `--no-gov` / `SLIM_GOV=0` |
| `stop` | Browser.close, then SIGTERM/SIGKILL of the browser's own process group; attached sessions are only forgotten |
| `status` | alive, pid, port, browser, profile, tabs, procs/pss/rss/uss of the process tree |
| `attach --port N` | adopt an existing browser (e.g. one cu started) as a session; never killed by `stop` |
| `sessions` | list saved sessions |
| `tabs` / `open [URL] [--context ID] [--background]` / `close [ID]` / `switch ID` | tab management; `open` and `switch` change the current tab |
| `nav URL [--wait load\|dom\|none]` / `back` / `forward` / `reload [--hard]` | navigation (waits for load by default) |
| `url` / `title` / `text [SEL]` / `html [SEL]` | read page |
| `eval JS\|-` | Runtime.evaluate (awaits promises, returns by value; `-` reads stdin) |
| `shot FILE [--full] [--selector S] [--format png\|jpeg\|webp] [--quality N]` | screenshot (`-` = stdout) |
| `pdf FILE [--landscape] [--background]` | print to PDF (headless only) |
| `click` / `dblclick` / `hover SEL` | Input.dispatchMouseEvent at the element center (scrolled into view) |
| `type SEL TEXT` | focus, then key events per character (keydown/keypress/input/keyup) |
| `fill SEL TEXT` | clear, Input.insertText, fire change |
| `press KEY` | Enter, Tab, Escape, Backspace, Arrow*, Home, End, PageUp/Down, F1-F12, any char, modifiers `Control+a`, `Shift+Tab` |
| `scroll DX DY` / `scroll-to SEL` | mouse wheel / scrollIntoView |
| `select SEL VALUE` / `check SEL [--off]` | form controls (option value or label) |
| `wait SEL [--visible] [--gone]` / `wait-load` / `wait-idle [--idle MS]` | waits (bounded by --timeout) |
| `cookies` / `cookie-set NAME VALUE [--domain D] [--path P] [--url U] [--secure] [--httponly]` / `cookies-clear` / `storage-clear` | storage |
| `viewport W H [--scale F] [--mobile]` \| `viewport reset`, `ua STRING\|--reset`, `offline on\|off`, `block [PATTERN...]` | emulation, saved in the session and re-applied on every attach |
| `download-dir DIR\|off` | Browser.setDownloadBehavior (re-applied on connect) |
| `frames` | frame tree |
| `console` | console messages, exceptions and log entries (buffered ones are replayed) for --timeout MS (default 1000) |
| `mem` | process-tree memory of the browser (iso = anon + shmem + file_union, the official metric; pss/rss/uss; governor pid) + JS heap per tab (heap is per renderer isolate, shared by tabs of one renderer) |
| `gov [--once] [--pid P] [--reclaim-s N] [--grow-mb N] [--idle-ms N] [--idle-cpu PCT] [--hibernate K] [--no-gc]` | memory governor in the foreground (events on stdout, JSON lines with `--json`); `--once` = one pass now, prints iso before/after. See docs/GOVERNOR.md |
| `purge` | HeapProfiler.collectGarbage on every tab + Memory.simulatePressureNotification critical |
| `freeze [ID]` / `thaw [ID]` | Page.setWebLifecycleState frozen/active |
| `context-new [--url URL]` / `context-close ID` / `contexts` | isolated browser contexts (incognito-like) |
| `cdp METHOD [PARAMS_JSON] [--target browser\|page] [--events MS]` | raw DevTools passthrough: any protocol method |
| `batch` | read commands from stdin, one per line (shell-like quoting, `#` comments), run over ONE connection, one JSON line each |
| `snapshot` / `find` / `inspect` | compact element tree, stable `@eN` refs and element details |
| `act JSON\|@file\|-` / `pipe` | action arrays / persistent NDJSON pipeline over one connection |
| `upload` / `drag` / `focus` / `submit` / `form fill` | additional form and element operations |
| `profile create\|list\|use\|save\|restore\|...` | named persistent user profiles and tab-session restore |
| `record start\|stop\|status\|list` | browser-tab video recording via screencast + runtime ffmpeg |
| `resize W H` / `responsive OUTDIR --sizes ...` | live viewport resize and multi-breakpoint screenshots |
| `protocol domains\|methods\|events\|show\|coverage` | live CDP schema discovery; `cdp-listen` collects events |
| `mcp` | stdio JSON-RPC MCP server with persistent in-process command invocation |

Selectors include CSS, `xpath=`, `text=`, `role=...`, `testid=`, `label=` and snapshot refs
(`@eN`). Snapshots traverse open shadow roots and frames where CDP exposes a context.
See docs/INTERACTION.md for staleness, actionability and pipeline semantics.

Notes:
- DevTools resets emulation overrides when a client disconnects, so `viewport`/`ua`/`offline`/`block`
  are active while slimium is connected (i.e. during every slimium command and batch). Use `batch`
  or `start -- --user-agent=...` / `--window` for settings that must hold between commands.
- `block` (Network.setBlockedURLs) blocks subresource requests, not top-level navigations.
- In `--port` mode nothing is persisted; the current tab is the first page target unless `--tab`.

### Wrapper: `slimium-chrome`

`slimium-chrome [chromium args...]` (or `slimium chrome ...`) execs the engine directly (no fork) with
the low-memory profile merged into the given argv — a drop-in browser for cu: `CU_BROWSER=slimium-chrome`.

| env | |
|---|---|
| `SLIM_CHROMIUM` | engine path (default: first of chromium-browser, chromium, google-chrome-stable, google-chrome in PATH) |
| `SLIM_PROFILE` | memory profile: `off\|balanced\|max` (default balanced) |
| `SLIM_USER_PROFILE` | named persistent website-login profile; overrides the caller's data dir |
| `SLIM_WRAPPER_LOG=FILE` | append the final argv (shell-quoted) |
| `SLIM_ENGINE_DIRECT=0` | do not bypass distro launcher scripts (Fedora's chromium-browser.sh adds flags and two `cat` processes; by default the sibling ELF is exec'd) |

| `SLIM_GOV=0` | plain exec: no governor, no scope, no engine copy, no fadvise |
| `SLIM_SCOPE=0` | do not wrap the engine in `systemd-run --user --scope` |
| `SLIM_ENGINE_COPY=0` | use the system engine files instead of the private (reflinked) copies in `~/.cache/slimium/engines` |
| `SLIM_ENGINE_COPY_LIBS=0` | do not use private copies of the system shared libraries (LD_LIBRARY_PATH into the copy slot; glibc never copied) |
| `SLIM_GOV_LOG=FILE`, `SLIM_GOV_*` | governor log (JSON lines) and knobs, see docs/GOVERNOR.md |

`--version` is passed through unchanged (engine's version line on stdout, no profile flags, no governor).

### Memory governor

By default `slimium-chrome` and `slimium start` keep the engine's pid but run it in its own systemd
user scope (`systemd-run --user --scope`), from a private copy of the engine ELF (a reflink on
btrfs/xfs). They also fork a detached `slimium gov` process. While the browser is idle (tree CPU
< 5 % for 5 s) it sends critical memory pressure + GC once per idle period. Then it evicts the
browser's cold file pages (mostly engine code) with cgroup v2 `memory.reclaim`
(`swappiness=0`: file pages only, no swap). It does this every 30 s, or sooner once 24 MB came back,
and only in cgroups whose processes all belong to the browser tree. Opt-in tab hibernation:
`SLIM_GOV_HIBERNATE=K`. Details, numbers and caveats: [docs/GOVERNOR.md](docs/GOVERNOR.md).
