# slimium memory governor

`src/gov.c` (+ `src/cmd_gov.c`, wiring in `src/cmd_session.c`). It keeps a running browser's
**isolated footprint** (iso = Pss_Anon + Pss_Shmem + union of resident file pages of the tree,
`src/mem.c`) low without touching what an agent (cu) is doing. The biggest part of iso after the
flags is the engine's code (the Chromium ELF has ~210 MB resident after start-up and the first
navigations, cold afterwards), and cgroup v2 proactive reclaim gives it back. See
docs/RESEARCH-structural.md §3.

## What runs where

`slimium-chrome ARGS` (cu's `CU_BROWSER`) and `slimium start` (unless `--no-gov` / `SLIM_GOV=0`):

1. **Private engine copy**: the engine ELF is copied to
   `~/.cache/slimium/engines/<name>-<dev>-<ino>-<size>-<mtime>[.N]/` with `copy_file_range` (a
   **reflink** on btrfs/xfs, so no extra disk). The other files of the engine dir (paks, icudtl,
   locales, crashpad, libs) are symlinks. The browser runs from that copy. Reason: page cache is
   charged to the cgroup that first faulted it, and on a machine where other Chromiums run (the
   user's cu, other agents) the system ELF's pages are charged to *their* cgroups. Those pages are
   also mapped, so `posix_fadvise` cannot drop them and our reclaim cannot evict them. Measured
   with the system ELF: a reclaim freed 18 MB and iso did not move. There is **one slot per
   concurrently running browser**: a slot is busy while a process has it as its exe, and up to
   `SLIM_ENGINE_COPIES`=4 slots are made. Two browsers sharing one copy would share each other's
   charges (measured: 470 MB steady instead of 316 MB). Copies of an older engine version are
   removed when the source changes. `SLIM_ENGINE_COPY=0` uses the system ELF.
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
  = 1000 ms of tree CPU, e.g. a cu action) re-arms the GC pass and an early reclaim. Chromium's
  own idle blips (timers, refaults after a reclaim: 6-13 % for a second) do not, which otherwise
  caused a full GC pass every 30 s. A 2-3 s grace period after each of our passes ignores the CPU
  they cause.
- **(a) Pressure + GC, once per idle period after real activity**:
  `Memory.simulatePressureNotification {"level":"critical"}` on the browser target, then for every
  page target: attach (flatten), the same pressure notification, `HeapProfiler.collectGarbage`,
  detach. **Never `Memory.forciblyPurgeJavaScriptMemory`**: it destroys the page's V8 contexts.
  The DevTools connection is opened per pass and closed afterwards (kept open only with
  hibernation).
- **(b) Reclaim, while idle**: on a later tick than the GC pass (after the grace period), then every
  `SLIM_GOV_RECLAIM_S` (30 s). It runs **sooner** once the mapped file pages (`memory.stat
  file_mapped`) of the cgroups reclaimed last time grew by `SLIM_GOV_RECLAIM_GROW_MB` (24),
  but never more often than every `SLIM_GOV_RECLAIM_MIN_S` (5 s). The trigger deliberately does
  not watch `file`: most of what comes back after a reclaim is readahead around refaults, which
  is charged but unmapped. A first version that watched `file` reclaimed every 5 s (88 reclaims
  in 4 min) for about the same iso.
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

Events are written as JSON lines to `SLIM_GOV_LOG` (start, idle, active, pass, reclaim, grown,
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

`tests/gov.sh` (in `make test`, also `make test-gov`; CDP 9440-9442, temp profiles/state): 49 checks.
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
reclaim happens, nothing goes to stdout, and the governor exits with the engine. The user's cu
fingerprint (daemon/browser pids, listeners 8810-8834/9362-9386) is unchanged.

cu suite through cu-slim with the governor live (`tests/cu/run.sh`): **71/71**.
