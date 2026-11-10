# slimium memory governor

`src/gov.c` (+ `src/cmd_gov.c`, wiring in `src/cmd_session.c`). It keeps a running browser's
**isolated footprint** (iso = Pss_Anon + Pss_Shmem + union of resident file pages of the tree,
`src/mem.c`) low without touching what an agent (cu) is doing. The biggest part of iso after the
flags is the engine's code (the Chromium ELF has ~210 MB resident after start-up and the first
navigations, cold afterwards), and cgroup v2 proactive reclaim gives it back. See
docs/RESEARCH-structural.md §3.

## What runs where

`slimium-chrome ARGS` (cu's `CU_BROWSER`) and `slimium start` (unless `--no-gov` / `SLIM_GOV=0`):

1. **Private engine copy**: every mapped file of the engine dir is copied to
   `~/.cache/slimium/engines/<name>-v2-<dev>-<ino>-<size>-<mtime>[.N]/` with `copy_file_range`
   (a **reflink** on btrfs/xfs, so no extra disk). That covers the ELF, `.pak` resources,
   `icudtl.dat`, `v8_context_snapshot.bin`, the bundled `.so` (libEGL, libGLESv2,
   libvk_swiftshader, libvulkan), `chrome_crashpad_handler` and `locales/*.pak`. The setuid
   `chrome-sandbox` and launcher scripts stay symlinks. The browser runs from that copy. Reason:
   page cache is charged to the cgroup that first faulted it, and on a machine where other
   Chromiums run (the user's cu, other agents) the system engine's pages are charged to *their*
   cgroups. Those pages are also mapped, so `posix_fadvise` cannot drop them and our reclaim
   cannot evict them. Measured with the system ELF: a reclaim freed 18 MB and iso did not move.
   There is **one slot per concurrently running browser**: a slot is busy while a process has it
   as its exe, and up to `SLIM_ENGINE_COPIES`=4 slots are made. Two browsers sharing one copy
   would share each other's charges (measured: 470 MB steady instead of 316 MB). Copies of an
   older engine version (or the v1 layout) are removed when the key changes.
   `SLIM_ENGINE_COPY=0` uses the system files.
   **System libraries** (`SLIM_ENGINE_COPY_LIBS`, default on with the governor): after the first
   reclaim of an engine version, the governor records which system `.so` files the tree maps in
   `<slot 0>/libs.list` (273 on Fedora 44: nss, gtk, glib, icu, harfbuzz, crypto, opensc, ...).
   Later launches reflink them into `<slot>/lib/` with their SONAME alias symlinks and export
   `LD_LIBRARY_PATH=<slot>/lib`. glibc's own libraries (libc, libm, ld-linux, libpthread,
   libdl, librt, libresolv, libnss_*, ...) are **never** copied, because a stale libc next to a
   newer system ld.so would break. A copy is redone whenever its source's size or mtime changes
   (checked on every launch), and libraries dlopened by absolute path (p11-kit modules) still come
   from the system. These copies hold ~50 MB of resident pages that are otherwise charged to the
   rest of the desktop; with them they are the browser's own and reclaimable.
   **Trade-off**: their hot part (~5-15 MB) is a real duplicate of pages the desktop already
   shares, so iso drops while real RAM can rise slightly on a busy desktop. `SLIM_ENGINE_COPY_LIBS=0`
   turns it off.
2. **fadvise**: `posix_fadvise(DONTNEED)` on the (copied) ELF and the `.so/.pak/.dat/.bin` +
   `locales/` next to it. This drops only *unmapped* cached pages, typically those left by a
   previous browser on the same slot whose scope is gone, so their charge sits in the parent
   slice. It is harmless to running browsers. `SLIM_GOV_FADVISE=0` disables it.
3. **Governor**: double fork (it is not a child of the browser and not in its tree). The new
   process runs `setsid`, ignores SIGHUP, puts stdio on /dev/null, closes every other fd and
   re-execs itself as `slimium gov --daemon --pid PID --pid-start T --cdp PORT|none [--udd DIR]
   [--wait-engine]`. That command line does not contain `--remote-debugging-port=`, so pid
   lookups by flag (slimium's `find_browser_pid`, cu) never pick it. The CDP port comes from
   `--remote-debugging-port=N` (0: `<user-data-dir>/DevToolsActivePort`; pipe or none: reclaim
   only). It exits when the browser pid dies (pid + /proc starttime, so pid reuse is safe).
4. **Own scope, same pid**: the wrapper probes `systemd-run --user --scope --quiet --collect
   --unit=slimium-probe-… -- true` (~17 ms), then **execs** `systemd-run --user --scope --quiet
   --collect --unit=slimium-PID-START -- ENGINE ARGS`. With `--scope`, systemd-run moves itself
   into the new scope and execs the command in place. So the engine keeps the pid cu spawned (cu
   still waits on its own child), and everything it forks lands in `slimium-PID-START.scope`.
   Chromium then moves its browser process (and what it starts later) into
   `app-org.chromium.Chromium-PID.scope`; the zygotes, renderers, GPU and storage processes stay
   in ours. The governor handles both. If systemd-run is missing or the probe fails, the engine
   runs unscoped. `SLIM_SCOPE=0` forces that. Unscoped, the shared cgroup (cu's, a terminal's)
   fails the foreign check and is never reclaimed; only Chromium's own app scope is.

`--version` / `--product-version` exec the engine directly: no copy, no scope, no governor.
Nothing is written to stdout or stderr. With `SLIM_GOV=0` the wrapper is exactly the old plain exec.

## Governor loop (1 s tick, nice 10)

- **Tree**: root + descendants from one /proc scan every 5 s. Also included are Chromium's
  crashpad handler (`--crashpad-handler-pid=N` on a zygote's command line), which is reparented
  to the user's systemd but lives in our scope, and its self-monitor (same parent, same cgroup,
  started within 1 s). Each tick reads only `/proc/PID/stat` of the known pids.
- **Idle** = tree CPU < `SLIM_GOV_IDLE_CPU` (5 % of one core) for `SLIM_GOV_IDLE_MS` (5000).
  CPU spent above the threshold accumulates as "busy". Only real activity (>= `SLIM_GOV_BUSY_MS`
  = 1000 ms of tree CPU, e.g. a cu action) re-arms the GC pass. Chromium's own idle blips
  (timers, refaults after a reclaim: 6-13 % for a second) do not, which otherwise caused a full
  GC pass every 30 s. A 2-3 s grace period after each of our passes ignores the CPU they cause.
- **Quiet** (since 0.3.2) = `SLIM_GOV_QUIET_S` (30) seconds since the last *real activity*: a
  stretch above the idle threshold that adds up to 500 ms of CPU. Measured, an idle browser with
  three parked tabs that poll makes stretches of 50-290 ms; a navigation, a scroll or a capture
  with what follows it, 500 ms and more. The GC pass and the reclaim wait for quiet, not just
  for idle. Until 0.3.1 they ran 5 s into every idle period, which between an agent's commands
  is every few seconds: on a cu flow of 38 commands 8 s apart that was 46.2 CPU-seconds of
  browser and 5.7 s of commands, against 34.1 and 3.9 with the quiet wait, for 386 MB of
  anonymous memory instead of 416 (the pass costs a full GC of every tab, and the command after
  a reclaim pays for the code that faults back in). `SLIM_GOV_QUIET_S=0` is the old timing.
- **(a) Pressure + GC, once per quiet period after real activity**:
  `Memory.simulatePressureNotification {"level":"critical"}` on the browser target, then for every
  page target: attach (flatten), the same pressure notification, `HeapProfiler.collectGarbage`,
  detach. **Never `Memory.forciblyPurgeJavaScriptMemory`**: it destroys the page's V8 contexts.
  The DevTools connection is opened per pass and closed afterwards (kept open only with
  hibernation).
  **Growth does not wait** (since 0.3.2). The governor follows the tree's anonymous memory (sum
  of `RssAnon`) against its level right after the last GC pass (or the lowest seen since):
  - grown by `SLIM_GOV_GC_GROW_MB` (256) and idle: a GC pass at once, without the quiet wait and
    without a reclaim (event `grown-gc`);
  - grown by `SLIM_GOV_BUSY_GROW_MB` (1024) and **not idle**: a GC pass anyway, at most every
    10 s (event `busy-gc`). A browser that is never idle (a page animating in front, a run of
    navigations) was never collected at all: 90 s of navigations of a development build, one a
    second, took the renderer from 0.4 to 3.2 GB (scope peak 3949 MB) with no pass; with this,
    four passes, the renderer between 0.5 and 1.1 GB, peak 1959 MB, and no fewer navigations.
  `0` turns either off.
- **(b) Reclaim, once quiet**: on a later tick than the GC pass (after the grace period), then every
  `SLIM_GOV_RECLAIM_S` (30 s). It runs **sooner** once the mapped file pages (`memory.stat
  file_mapped`) of the cgroups reclaimed last time grew by `SLIM_GOV_RECLAIM_GROW_MB` (24),
  but never more often than every `SLIM_GOV_RECLAIM_MIN_S` (5 s). The trigger deliberately does
  not watch `file`: most of what comes back after a reclaim is readahead around refaults, which
  is charged but unmapped. A first version that watched `file` reclaimed every 5 s (88 reclaims
  in 4 min) for about the same iso.
  **Back-off** (since 0.3.2): a reclaim that no real activity called for doubles both waits for
  the next one, up to `SLIM_GOV_RECLAIM_BACK_MAX` (64) times; real activity resets it. Evicting
  the engine's code pays when it stays out. A browser with a tab that polls touches it again at
  once and it is read back from disk: three parked tabs of that kind, 90 s, nobody using the
  browser, cost 1666 MB of disk reads and 1280 major faults to hold 180 MB of page cache instead
  of 490 MB. With the back-off: 682 MB in the first 90 s, 1183 MB over 5 minutes with no reclaim
  at all in the last windows, the file pages settling at about 275 MB. `1` is no back-off.
  Per cgroup of the tree (`/proc/PID/cgroup`, v2 `0::` line, never `/`):
  1. **foreign check**: every pid in `cgroup.procs` of the cgroup *and of every descendant
     cgroup* (memory.reclaim acts on the whole subtree) must be in the governed tree. If not:
     event `skip-foreign`, untouched;
  2. amount = `active_file + inactive_file` from `memory.stat`;
  3. write `"<64M chunk>K swappiness=0"` to `memory.reclaim`, **re-checking the foreign
     condition before every chunk**. EAGAIN means nothing more is reclaimable: done. EINVAL means
     the kernel has no `swappiness=`, so reclaim is turned off (it would otherwise swap anon).
     If the tree's CPU exceeds max(4 x idle_cpu, 20 %) during the reclaim (judged over >= 250 ms),
     it stops and leaves the browser alone.
  `swappiness=0` means file pages only; anon is never swapped (swap stayed 0). Evicted pages fault
  back from disk when used.
- **(c) Hibernation, opt-in** (`SLIM_GOV_HIBERNATE=K`, `--hibernate K`; **off by default because
  in-memory JS state is lost**): with a persistent browser connection and
  `Target.setDiscoverTargets`, the governor tracks per tab its last activity (any
  targetInfoChanged not caused by us) and whether a client is attached. In each idle pass, http(s)
  and file tabs beyond the K most recent, untouched for `SLIM_GOV_HIBERNATE_MIN_S` (120 s) and
  with **no client attached**, are hibernated: the current history entry id is saved, then
  `Page.navigate about:blank`. When a client attaches to a hibernated tab (`attached` false ->
  true, our own attaches are tracked per tab), the governor restores it with
  `Page.navigateToHistoryEntry`. Form values and scroll come back from history; JS heap state
  does not. On SIGINT/SIGTERM of a foreground governor every hibernated tab is restored.
  **Caveat**: the first command of the client that triggers the restore races with it and can
  still see about:blank (tests/gov.sh: the first `url` returns about:blank, the next one the real URL).
  **With the session's emulation keeper** (docs/EMULATION.md), every tab is always attached. The
  keeper marks itself in `<udd>/.slimium-keeper`. While it lives, "no client attached" is not
  required (age/recency only). Hibernated tabs are listed in `<udd>/.slimium-hibernated`, and the
  next slimium command for such a tab restores it itself before running (no race). The governor
  logs it as `restore ... "by":"client"`.

Events are written as JSON lines to `SLIM_GOV_LOG` (start, idle, active, pass, reclaim, grown,
grown-gc, busy-gc,
skip-foreign, skip-no-reclaim, hibernate, restore, no-cdp, exit). `SLIM_GOV_MEASURE=1` adds iso
before/after each pass (two pagemap scans of the tree, ~50-100 ms; off by default).

## CLI

```sh
slimium gov [--session S | --port N [--pid P]] [--once] [--reclaim-s N] [--grow-mb N] [--idle-ms N]
            [--idle-cpu PCT] [--hibernate K] [--no-gc] [--json]
slimium --session s gov --once      # iso 523.5 -> 321.9 MB (anon+shmem 218.9 -> 221.1, file_union 304.7 -> 100.8);
                                    # reclaimed 304.4 MB in 2/2 cgroups (0 foreign skipped); gc 14 tabs, pressure sent; 2052 ms
slimium start [--no-gov]            # prints ", governor pid N"; status/mem show iso + governor pid
```
Foreground mode prints the events (text, or JSON with `--json`) until the browser exits.

## Tests

`tests/gov.sh` (in `make test`, also `make test-gov`; CDP 9440-9442, temp profiles/state): 58 checks.
When a pass runs: idle but not quiet and nothing grown, none; grown by more than
`SLIM_GOV_GC_GROW_MB` and idle, a `grown-gc` pass and no reclaim; grown and kept busy by a page
that spins, a `busy-gc` pass with no idle period in between.
Session start spawns a detached governor (not a child of the browser; command line without
`--remote-debugging-port`), status/mem show iso and the governor pid, and the zygotes run in
`slimium-PID-*.scope`. A concurrent second browser (the wrapper) gets a different engine copy. A
pressure+GC pass and a reclaim of the browser's scope appear in the log. Every reclaimed cgroup is
`slimium-PID-*` or `app-org.chromium.Chromium-PID.scope`, never the test's own cgroup or
cu-daemon. **A foreign process injected into the browser's scope** makes `gov --once` skip that
cgroup (`skip-foreign`), and once it is gone the scope is reclaimed again. Hibernation:
hibernate, restore on attach, original URL back, all restored on SIGTERM. Every tab evaluates
`1+1` after reclaim. The render fixture screenshot matches the exact pixel values (CSS, canvas,
SVG, WebGL, img, transforms) after reclaim. With **51 tabs** the oldest tab still answers
(`eval`, timers) after a GC pass over all of them. Governor PSS < 2 MB and anon < 1 MB. The
governor exits within 10 s of the browser. An unscoped launch (`SLIM_SCOPE=0`) is skipped as
foreign and its cgroup never reclaimed. Wrapper: `--version` spawns no governor, the engine keeps
the pid the caller spawned, the governor is detached, the zygote scope is `slimium-PID-*`, a
reclaim happens, nothing goes to stdout, and the governor exits with the engine. A later launch maps the private library copies (never glibc) and evaluates. The user's cu
fingerprint (daemon/browser pids, listeners 8810-8834/9362-9386) is unchanged.

cu suite through cu-slim with the governor live (`tests/cu/run.sh`): **71/71**.

## Numbers, round 3 (2026-11-10): the 0.3.2 timing

What the quiet wait, the growth triggers and the reclaim back-off change, each measured against
the 0.3.1 timing (`SLIM_GOV_QUIET_S=0 SLIM_GOV_GC_GROW_MB=0 SLIM_GOV_BUSY_GROW_MB=0
SLIM_GOV_RECLAIM_BACK_MAX=1`) on the same binary, one run each unless said.

| | 0.3.1 timing | 0.3.2 timing |
|---|---|---|
| `tests/cu/gov-ram.sh` (slim, 13-tab flow): steady median / min, iso MB | 224.4 / 213.4 | 228.2 / 217.6 |
| same: time-weighted mean over the 150 s; 30 s after the active pass | 232.9; 258.2 | 286.2; 415.6 |
| same: GC passes / reclaims in the run | 2 / 8 | 1 / 4 |
| cu flow, 38 commands over three apps 8 s apart: browser CPU; time in commands | 46.2 s; 5.7 s | 34.1 s; 3.9 s |
| same: anonymous memory, mean / max MB | 386 / 502 | 416 / 527 |
| 90 s of navigations, one a second: renderer high-water; scope peak | 3.2 GB; 3949 MB | 1.1 GB; 1959 MB |
| unused browser, three parked tabs that poll, 90 s: disk reads; major faults | 1666 MB; 1280 | 682 MB; 636 |
| same, 5 minutes (0.3.2 only): disk reads; file pages at the end | | 1183 MB; ~275 MB (490 MB with no reclaim) |

The steady footprint is the same. What is given up is how soon it is reached: the first pass
comes 30 s after the last real activity instead of 5 s into every pause, so a reading taken
less than 30 s after a burst is the working set, not the cleaned one. What is gained is that
an agent's pauses no longer cost a full GC and cold code each, that a browser which is never
idle is collected at all, and that an unused browser stops reading its own code back from
disk.

## Numbers, round 2 (2026-11-05/06): all engine files + system libraries copied, lite-mode in max only

Same protocol as below (`tests/cu/gov-ram.sh`, cu-slim, 13-tab flow, 150 s steady of 5 s samples).
The configs alternated ABAB: balanced+gov, max+gov, cu-fast stock. The runs are listed with
their steady median / time-weighted mean / active peak in MB.

| config | runs (median / tw-mean / peak) | **median of runs** | settled | ratio vs cu-fast stock | ratio vs plain stock |
|---|---|---|---:|---|---|
| stock, cu fast mode | 536.0/543.5/590, 532.3/539.5/595, 517.4/526.8/556 | **532 / 540 / 590** | 574-585 | 1 | 0.75x |
| stock-compat (plain Chromium; 1 run, round 1) | 709/706/751 | **709 / 706 / 751** | 722 | | 1 |
| **balanced + governor** | 230.2/233.8/364, 227.6/232.7/368, 219.8/228.5/368, 223.4/240.2/371 | **225.5 / 233 / 368** | 496-498 | **0.42x / 0.43x / 0.62x** | **0.32x / 0.33x / 0.49x** |
| **max + governor** | 220.6/233.3/352, 215.4/220.9/348, 211.9/218.9/347 | **215 / 221 / 348** | 485 | **0.40x / 0.41x / 0.59x** | 0.30x / 0.31x / 0.46x |
| balanced minus `--max-semi-space-size=1` + governor | 223.6/229.4/364, 217.7/227.3/358 | 221 / 228 / 361 | 496-498 | 0.42x / 0.42x | |

- In every steady series the "max" is the settled sample at t=0, before the first idle pass:
  496-498 (balanced) and 485 (max) vs 574-585 for stock.
- **Steady is under 0.5x of the strict baseline** (cu-fast stock) for both levels, by median and
  by time-weighted mean, at about 0.42x. It went from 0.52x to 0.42x through: all engine files
  copied (-8 MB), system libraries copied (-40 MB), and lite-mode no longer making a difference.
- `--max-semi-space-size=1` is neutral here (221/228 without it vs 225/233 with it, within the
  run-to-run spread of ±6 MB). It stays in balanced (it is the memory track's measured setting
  for workload A).
- Latency with library copies (5 rounds): cold after a reclaim snapshot 73/120, click 38/41,
  shot 402/536 ms; warm 5/8, 5/5, 485/486 ms. The same as without the library copies.
- **Standalone** (`bench/gov-standalone.sh`, workload A: 6 content-heavy pages on 6 loopback
  hosts, `slimium start` balanced + governor vs the stock ELF with no flags, crashpad included):
  slimium steady median/tw-mean 410.2/414.6 and 410.3/412.6 MB; stock 828.2/830.2 and 836.3/838.2
  MB. That is **0.49x / 0.50x**. Settled: 647 vs 884-898.
- What is left right after a reclaim (13 tabs, balanced, per-path union of mapped pages): 17-19 MB
  in total. That is fonts 7 MB, glibc and other uncopied system files 4.2 MB, the hot part of the
  libcrypto copy 2.1 MB, profile files 1.3 MB and Widevine 1 MB. 20 s later it is 42 MB (+23 MB of
  hot engine code coming back). Nothing else in our control remains; the rest of iso is anon
  (~195-230 MB: renderer + browser heaps).

## Numbers (2026-11-05, Chromium 156.0.8078.79, Fedora 44, kernel 7.2, cu 1.9.0 + slimium-integration)

**Through cu** (`tests/cu/gov-ram.sh`). Each run is a cu-slim daemon with a temp dir, 10 leased
fixture tabs plus example.com, en.wikipedia.org/wiki/Web_browser and news.ycombinator.com, and a
`cu snapshot` in each (13/13 every run). *Settled* = 4 s after the flow (the governor has not
acted yet). *Steady* = 31 samples, 5 s apart over 150 s (min / median / max / time-weighted
mean). *Active peak* = max of 1 s samples during an active pass (snapshot + screenshot of all 13
tabs + one navigation). *After* = 30 s after that. iso is in MB, tree only (crashpad not
included); the user's cu was running the whole time (fingerprint unchanged).

| config | settled | steady min / median / max / tw-mean | active peak | after 30 s | reclaims / gov CPU |
|---|---:|---|---:|---:|---|
| stock-compat: stock ELF, `CU_MODE=compat` (plain Chromium) | 722 | 688 / 709 / 722 / 706 | 751 | 755 | - |
| stock: stock ELF, cu fast mode (cu's own lean flags) | 582-587 | 520-534 / 520-537 / 558-587 / 525-544 (3 runs) | 534-592 | 539-592 | - |
| **slimium balanced (main 2960a03) + governor** | 495 | **265 / 274 / 495 / 280** | 410 | 303 | 17 / 1 s in ~5 min |
| same, balanced without `--lite-mode` | 497 | 264 / 275 / 497 / 279 | 416 | 300 | 21 / 2 s |
| same, `SLIM_GOV_RECLAIM_GROW_MB=12` | 492 | 263 / 270 / 492 / 276 | 405 | 287 | 26 / 2 s |
| slimium balanced (pre-merge profile) + governor | 547-556 | 310-311 / 318-321 / 553-556 / 328-336 (3 runs) | 456-475 | 333-363 | 16-33 |
| same + `SLIM_GOV_HIBERNATE=3` | 561 | 317 / 324 / 561 / 334 | 468 | 341 | 0 tabs hibernated: cu keeps a client attached to its leased tabs |
| pre-merge, two browsers sharing ONE engine copy (bug, fixed by slots) | 558 | 469 / 472 / 558 / 475 | 518 | 499 | - |

Ratios of steady median / tw-mean (the "max" of steady is the settled sample at t=0, before the
first idle pass):
- vs **plain stock Chromium** through cu (compat, 709 / 706): **0.39x / 0.40x**.
- vs **cu's fast-mode stock** (525 / 534 in the same session): **0.52x / 0.52x**. Without the
  governor, slimium settles at 495 = 0.85x of 582 there. The governor is what takes it to 0.52x.
- Active peak 410 vs 570 (0.72x). 30 s after activity 303, back to ~275 at the next reclaim.
- `--lite-mode` makes no measurable difference on this cu flow with the governor (274 vs 275).
  The flow is DOM- and layout-heavy, not JS-heavy.
- A tighter growth trigger (12 MB) buys ~4 MB for twice the reclaims: not worth it as a default.

**Action latency through cu** (5 rounds on the form tab, median / max ms; *cold* = right after a
governor reclaim, stock = after 30 s idle; *warm* = immediately again):

| | snapshot | click | shot |
|---|---|---|---|
| slimium cold (after reclaim) | 61 / 116 | 35 / 42 | 383 / 517 |
| slimium warm | 4 / 5 | 4 / 4 | 489 / 490 |
| stock cold (30 s idle) | 5 / 6 | 5 / 6 | 490 / 518 |
| stock warm | 4 / 4 | 4 / 5 | 487 / 492 |

The first action after a reclaim pays the refaults: +30-110 ms once. Screenshots (~0.4-0.5 s in
cu on any browser) do not get slower.

**Own sessions** (`slimium start`, same 13 tabs):
- `gov --once`: iso 523.5 → 321.9 MB (pre-merge max profile); 471 → 260 MB (merged balanced).
- Loop with the activity gate: right after each reclaim 340-356 MB, before the next one 358-440 MB
  (pre-merge max, 30 s timer + first adaptive version).
- **fadvise matters** (5 file:// tabs, merged balanced, the private copy slot reused after a
  previous browser):
  | | iso before → after one pass | file_union after |
  |---|---|---|
  | copy, no fadvise | 399 → 397 | 253 MB (cached pages still charged to the dead previous scope) |
  | copy, fadvise | 398 → **205** | 62 MB |
  | system ELF (user's cu and other agents run the same ELF) | 399 → 398 | 253 MB |
  So the private copy and the fadvise are both needed on a shared machine. Without them the
  governor does nothing for the file pages.
- What stays after a reclaim (~60-120 MB of file pages): the engine's hot set (27-80 MB) and a
  tail of system libraries (libc, glib, nss, icu, fonts) charged to other cgroups. Copying the paks
  as well would not help (icudtl/resources.pak are ~2 MB resident).

**Cost**: a reclaim takes 5-260 ms of kernel time in the governor (nice 10), ~16-20 per 5 minutes
idle with the default trigger, ~0.3-0.6 % of one core in total. A pressure+GC pass over 14 tabs
takes 0.35-0.45 s, once per idle period after real activity. The governor process has PSS
0.4-0.55 MB and anon 0.3-0.42 MB; its RSS is 2.3-2.4 MB, of which ~2 MB is shared libc/ld pages
(the same as any slimium command, whose peak RSS is 2.36 MB in tests/run.sh).

## Caveats

- **iso counts mapped pages only.** After a reclaim the browser's refaults also pull readahead
  into the page cache. That cache is charged to the browser's scope but not mapped, so it is not
  in iso (it is reclaimable and the next pass drops it). memory.stat `file` of the scopes can be
  100-260 MB above what iso shows.
- Pages the tree maps but that are charged to other cgroups (system libraries; the system ELF if
  `SLIM_ENGINE_COPY=0`) are never reclaimed by design. On a machine where slimium is the only
  Chromium, the system ELF would be reclaimable too.
- The first cu action after a reclaim is ~30-110 ms slower (refaults from disk; NVMe here).
- Two pressure+GC passes may hit a tab at a bad moment (a 10 s timeout per call, never
  forciblyPurge). tests/gov.sh checks that the oldest of 51 tabs still evaluates and runs timers
  afterwards.
- Hibernation is off by default. cu keeps its leased tabs attached, so it never hibernates them,
  and the first command after a restore can race it. It is meant for long-lived browsers with
  many idle tabs and no attached clients.
- The engine scopes are user systemd scopes: they leave cu-daemon.service's cgroup (as Chromium's
  own app-org.chromium.Chromium-PID.scope already does), so `systemctl --user stop` of a
  daemon's unit no longer kills the browser by cgroup. cu closes its browser itself, and the
  zygote and renderers exit with it.
- Up to 4 engine copies (`SLIM_ENGINE_COPIES`) of 322 MB each are reflinks on btrfs/xfs; on
  ext4 they are real copies (~0.3 s and 322 MB of disk each, made once per engine version).
- Needs cgroup v2, `memory.reclaim` with `swappiness=` (Linux >= 6.x) and a delegated user
  slice (systemd). Without them the governor still does pressure + GC.