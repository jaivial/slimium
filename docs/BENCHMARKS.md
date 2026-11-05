# slimium RAM benchmarks

Everything here is reproducible with `bench/` (memory track). Raw results:
`bench/results/raw.jsonl` (one line per run, with every sample and a per-process-type
breakdown), `smoke.jsonl`, `fidelity.jsonl`, `governor.jsonl`, the batch scripts
(`bench/results/batch*.sh`) and their logs. Final numbers are from batches 7 and 8
(profiles as committed); earlier batches are kept for the exploration history.

## Machine and engines

- Fedora 44, kernel 7.2.13, 16 cores, 59 GB RAM (~46 GB available), other agents'
  Chromium instances running concurrently (that is why the metric below exists).
- Engine A (default): Fedora Chromium 156.0.8078.79, the ELF
  `/usr/lib64/chromium-browser/chromium-browser` (not the `/usr/bin/chromium-browser` bash wrapper).
- Engine B (opt-in): Chrome for Testing `chrome-headless-shell` 156.0.8078.4
  (`~/.cache/ms-playwright/chromium_headless_shell-1248/`).

## Method

`bench/bench.sh run CONFIG WORKLOAD [REPS]` / `bench.sh ab CFG_A CFG_B WORKLOAD [PAIRS]`:

1. Launch the engine headless (`--headless=new`; `--headless` for the headless shell) with a temp
   `--user-data-dir` under /tmp and `--remote-debugging-port` in 9330-9359, in its own process
   group. The argv has the shape cu uses (port, flags, profile dir, headless, `about:blank`);
   `@merge LEVEL` configs run it through `slim_profile_merge_argv` exactly like `slimium-chrome`.
2. Open the workload, one tab per URL, with `PUT /json/new?URL`; wait until every opened tab has left
   `about:blank` and reports `readyState == complete` (20 s cap for A, 45 s for B); then wait 8 s.
3. Sample the whole tree 3 times, 2 s apart (`bench/slim-memstat --crashpad`: browser + all
   descendants + the two reparented crashpad handlers); keep the median sample.
4. Stop the process group, delete the profile.

`ab` alternates the two configs (A,B,A,B,...) so drift and other agents' load hit both equally.

**Metric — iso (isolated footprint)** = Pss_Anon + Pss_Shmem + union of the resident file-backed
pages mapped anywhere in the tree (`/proc/PID/maps` + `/proc/PID/pagemap` present and file-page
bits, one bitmap per (device, inode), `src/mem.c`). It is what the tree would occupy alone on the
machine. Raw PSS is also recorded but is not used for decisions: the engine's file pages are shared
with ~60 other Chromium processes on this machine, so PSS swings by ±40 MB between identical runs and
favours whichever binary other people happen to run. `anon` is listed because it is the part flags
can actually change.

Workloads:

- **A (offline, deterministic)**: `bench/www/` served by `bench/slim-httpd` on 7 loopback addresses
  (each a different site for Chromium's process model): a 120-section text article, a JS-heavy SPA
  (200k-object model, 8 MB typed array, 50k closures, 3000-row table re-rendered by a timer), a canvas
  gallery (1600x1200 canvas + 24 generated PNG images + SVG), a portal with a cross-site and a same-site
  iframe, a 20 000-item list, a 300-field form. Plus the initial `about:blank` = 7 tabs.
- **B (real web)**: en.wikipedia.org/wiki/Linux, github.com/torvalds/linux, bbc.com/news, youtube.com,
  developer.mozilla.org JavaScript docs, an ebay.com category page (eBay answers headless clients with
  its error page, in every config). Live content: expect ±5%.

Configs (`bench/configs/`): `stock` = `--no-first-run` only (plus the harness's headless/port/profile);
`stock-script` = same through the Fedora wrapper script; `cu` = cu's FastTest argv from
`~/cu/src/policy.rs` (incl. its `--renderer-process-limit=4`, `--disable-gpu`, stealth `--user-agent`);
`balanced`, `max` = stock argv merged with the profile; `cu+balanced` = cu's argv through the
merge (what `CU_BROWSER=slimium-chrome` runs); `hs-*` = the headless-shell engine.

## Results (iso, median, MB)

Headline: against the stock Chromium ELF, **balanced = 0.70x (A) / 0.74x (B)** with 0.0% screenshot
difference; **balanced on the opt-in chrome-headless-shell engine = 0.52x (A) / 0.64-0.67x (B)**, and
0.496x on A after one governor pass (GC + pressure + freeze). Flags alone do not reach 0.5x on the
default ELF: what remains is the pages' live data in one renderer and ~255 MB of resident engine
code/data (see "Where the memory goes"); the gov track's cgroup file-page reclaim addresses the latter.

### Workload A (offline)

| config | engine | runs | iso | anon | PSS | procs | iso vs stock ELF |
|---|---|---|---|---|---|---|---|
| stock | ELF | 9 | 914 (901-952) | 617 | 774 | 21-22 | 100% |
| stock-script (Fedora wrapper) | ELF | 2 | 912 | 621 | 745 | 23-24 | 99% |
| cu (FastTest flags) | ELF | 2 | 779-790 | 503 | 608 | 13 | 86% |
| **balanced** | ELF | 3 | **646** (643-647) | 386 | 486 | 9 | **70%** |
| max | ELF | 6 | 633-686 (bimodal) | 372-424 | 523 | 9 | 69% low mode (see note) |
| **cu+balanced** | ELF | 2 | 650-698 | 386-435 | 455-501 | 9 | **71-76%** (0.82-0.88x of cu) |
| hs-stock | headless shell | 2 | 636 | 478 | 625 | 13 | 69% |
| **hs-balanced** | headless shell | 3 | **476** (475-476) | 328 | 465 | 6 | **52%** |
| hs-max | headless shell | 2 | 468 (467-468) | 319 | 457 | 6 | 51% |
| cu-hs+balanced (cu argv) | headless shell | 2 | 475-477 | 329 | 465 | 6 | 52% |

Direct ABAB balanced vs max (batch 9): 649/633, 697/684, 647/686 — max is ~14 MB lower when both
runs are in the same mode.

Note on bimodality: on the ELF, some runs sit ~45 MB higher in anon (e.g. max 639 vs 684,
cu+balanced 650 vs 698) — uncollected garbage at sampling time: one `HeapProfiler.collectGarbage`
per page brings such a run from 698 to 640 (governor table). Medians of 2-3 runs can land on either
mode; the low mode is the steady state after a GC.

### Workload B (real web)

| config | runs | iso | anon | procs | iso vs stock |
|---|---|---|---|---|---|
| stock (ELF) | 6 | 838 (825-848) | 524 | 22-23 | 100% |
| **balanced** (ELF) | 3 | **624** (624-625) | 334 | 10 | **74%** |
| max (ELF) | 2 | 606-607 | 316 | 10 | 72% |
| hs-stock | 1 | 771 | 552 | 13 | 92% |
| **hs-balanced** | 2 | **540-559** | 333 | 6 | **64-67%** |
| cu (ELF, cu's stealth UA) | 1 | 981 | 660 | 14 | 117% (sites serve full desktop pages to a non-headless UA) |
| cu+balanced | 1 | 827 | 517 | 11 | 0.84x of cu |

One balanced B run (batch 7, rep 2) is excluded: BBC and eBay had not committed their navigation
when the old readiness check passed (titles empty); the check now requires every opened tab to have
left `about:blank`, and `report.sh` drops runs with an empty title.

### 56 tabs (smoke: workload A + 50 `Target.createTarget` tabs + contexts)

| config | iso | anon | procs |
|---|---|---|---|
| stock (ELF) | 2191 | 1845 | 73 |
| balanced (ELF) | 1155-1285 | 873-998 | 10 |
| max (ELF) | 1156 | 879 | 10 |
| hs-balanced | 1330-1444 | 948-1035 | 7 |

Loading 50 tabs (inner.html x50): stock 10.5 s, balanced 5.0 s — one renderer is not slower there.

### Runtime governor (`bench/governor.sh balanced A`, iso MB)

| step | iso | anon |
|---|---|---|
| settled | 698 | 436 |
| `HeapProfiler.collectGarbage` in every page | 640 | 378 |
| + `Memory.simulatePressureNotification critical` (browser + pages) | 637 | 375 |
| + `Page.setWebLifecycleState frozen` for background tabs | 637 | 375 |
| + background windows minimised (`Browser.setWindowBounds`) | 637 | 375 |
| + pressure + GC again | 629 | 367 |
| 30 s later | 628 | 367 |

All 7 tabs answered `Runtime.evaluate` after resume. GC is worth up to ~60 MB when garbage is pending,
pressure ~3-10 MB, freezing/minimising nothing measurable (older headless tabs are already
`visibilityState == hidden`). `Memory.forciblyPurgeJavaScriptMemory` is not used: it destroys the page's
V8 contexts. Core implements this as `slimium purge` (GC + critical pressure).

### Screenshot fidelity (`bench/fidelity.sh CONFIG`, % of pixels differing by > 24 in any channel vs stock ELF)

Fixtures at 1000x800, DPR 1: text + web font (OTF), images (PNG/SVG/background), canvas 2D (viewport
and element clip), SVG (gradient/filter/text), CSS (gradients, conic, transforms, shadows, blur,
opacity), a WebGL triangle, a 3624 px page captured full-page, en.wikipedia.org/wiki/Linux.

| config | text | images | canvas | canvas clip | svg | css | webgl | full page | wikipedia | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| balanced | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | identical |
| max | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | identical |
| cu+balanced | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | identical |
| hs-balanced / hs-max / cu-hs+balanced | **0.596** | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | glyph AA of CFF/OTF fonts differs (web font + monospace lines); TrueType/system text identical |

WebGL works in every config (no profile disables the GPU process; title "Fid WebGL ok").

### Functional smoke (`bench/bench.sh smoke CONFIG`)

Checks: all 7 tabs listed with the right titles; `Runtime.evaluate` (1+1) on every tab; non-blank
screenshot; `Target.createBrowserContext` + `createTarget` + flat attach + evaluate; a context with
`proxyServer`; cross-site `Page.navigate` twice inside the context; `Input.insertText` + mouse click;
download via `Browser.setDownloadBehavior`; 50 extra tabs via `Target.createTarget`; **the 8 oldest
(background) tabs must still answer `Runtime.evaluate`** (regression check: `--enable-low-end-device-mode`
froze them); browser alive.

PASS (batch 7): balanced, max, hs-balanced, cu+balanced; stock and cu pass too. Rejected by this check:
any profile with `--enable-low-end-device-mode` (background-tab eval hangs).

### Where the memory goes (balanced, A, per process type)

browser ~62 MB anon on the ELF (18 on the headless shell), one renderer ~280 MB anon (the pages' live
V8 + Blink data), GPU process ~16-21, storage service ~6, zygotes ~9-21, file union ~255 MB on the ELF
(~127 on the headless shell). Flags cannot shrink the pages' live data or the resident engine code;
the next levers are the engine binary and cgroup file-page reclaim (gov track).

## Exploration history (raw PSS, batches 1-2, workload A)

Single switches on top of stock (2 runs each) mostly stayed inside the ±40 MB PSS noise (stock
719-790); clear effects: `--single-process` 584-619 (6 procs; rejected, breaks
createBrowserContext), `--no-zygote --no-sandbox` 962-965 (worse), `NetworkServiceInProcess2` 676-709
(-1 proc), `--in-process-gpu` -1 proc. Ablation from a profile P0 holding every safe group (473-501 MB
PSS, 8 procs), removing one group at a time (3 runs): process model +140 MB, NetworkServiceInProcess2
+55, WebUI popups + component extensions +45, background features ~+35, ext/background-networking
~+30, `--lite-mode` ~+25 (with low-end mode on, which already enables lite mode), low-end mode ~0,
BackForwardCache/spare renderer ~0, and removing `--in-process-gpu --disable-gpu` **-25** (keeping the
GPU process is cheaper). Raw lines: `results/raw.jsonl` tags `explore`, `ablation`, `batch2`.

## Reproduce

```
make -C bench            # tools -> build/bench/
make -C bench test       # merge unit tests
bench/run.sh A 3         # = make bench: stock vs balanced (ABAB) + max, summary table
bench/bench.sh ab stock balanced A 3
bench/bench.sh smoke balanced
bench/fidelity.sh balanced
bench/governor.sh balanced A
bench/report.sh [TAG]    # markdown table from results/raw.jsonl
```
