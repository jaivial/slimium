# slimium

A Chromium-based browser for AI agents, written in C: the Chromium engine under a low-memory
process model, with every operation available from the command line, built to be cu's browser.

Status: under construction. See AGENTS.md for the rules and layout.

## Build

```sh
make                 # ./slimium (+ ./slimium-chrome symlink); C11, libc only, gcc -Os, stripped (~100 KB)
make test            # unit tests + functional suite (starts a headless browser on port 9310)
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
| `start [--profile off\|balanced\|max] [--headful] [--user-data-dir D] [--engine PATH] [--window WxH] [-- ARGS]` | launch detached (setsid, log file); default headless, port 9230-9361 (skips 9222, 9362-9386), data dir `~/.local/share/slimium/<session>` |
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
| `mem` | process-tree memory of the browser + JS heap per tab (heap is per renderer isolate, shared by tabs of one renderer) |
| `purge` | HeapProfiler.collectGarbage on every tab + Memory.simulatePressureNotification critical |
| `freeze [ID]` / `thaw [ID]` | Page.setWebLifecycleState frozen/active |
| `context-new [--url URL]` / `context-close ID` / `contexts` | isolated browser contexts (incognito-like) |
| `cdp METHOD [PARAMS_JSON] [--target browser\|page] [--events MS]` | raw DevTools passthrough: any protocol method |
| `batch` | read commands from stdin, one per line (shell-like quoting, `#` comments), run over ONE connection, one JSON line each |

Selectors are CSS; `//...` or `xpath=...` selects by XPath. Elements inside iframes: use `--tab`
with the frame's target or `cdp`.

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
| `SLIM_PROFILE` | `off\|balanced\|max` (default balanced) |
| `SLIM_WRAPPER_LOG=FILE` | append the final argv (shell-quoted) |
| `SLIM_ENGINE_DIRECT=0` | do not bypass distro launcher scripts (Fedora's chromium-browser.sh adds flags and two `cat` processes; by default the sibling ELF is exec'd) |

`--version` is passed through unchanged (engine's version line on stdout, no profile flags).
