# slimium memory profiles

`src/profile.c` holds the Chromium switch lists for the three levels and the
switch-aware merge that `slimium start` and `slimium-chrome` use. Every switch
below was measured on Chromium 156 (Fedora ELF `/usr/lib64/chromium-browser/chromium-browser`
156.0.8078.79 and Chrome-for-Testing `chrome-headless-shell` 156.0.8078.4) with the
benchmark in `bench/` (method and tables: [BENCHMARKS.md](BENCHMARKS.md)). A switch
that measured no effect was dropped, except a few that only cut background traffic
(listed as such).

Metric: **iso** = isolated footprint of the whole browser process tree = Pss_Anon +
Pss_Shmem + union of the resident file pages the tree maps (`src/mem.c`). Raw PSS
is not used for decisions: ~60 other Chromium processes on the machine share the
engine's file pages, which makes PSS swing by ±40 MB between identical runs.

## Levels

| level | what | workload A iso (ELF) | vs stock ELF |
|---|---|---|---|
| off | caller's argv untouched | 901-952 MB (stock, median 914) | 1.00x |
| balanced | all safe switches below except `--lite-mode` | 766-775 MB without the governor (643-647 with lite-mode) | 0.84x (0.70x) |
| max | balanced + `--lite-mode` + PartitionAlloc hardening off | 633-686 MB (bimodal; -14 MB vs balanced+lite in the same mode, ABAB) | 0.69x (low mode) |

**2026-11-05 (gov track): `--lite-mode` moved from balanced to max.** With the memory governor
(docs/GOVERNOR.md), which reclaims the engine's file pages while idle, lite-mode bought nothing
measurable through cu (steady median 274 vs 275 MB on the cu 13-tab flow). It still costs CPU-bound
JS 2.5-4x. Without the governor (`SLIM_GOV=0`), lite-mode is worth ~-120 MB on workload A: use max.
| balanced on chrome-headless-shell (opt-in engine) | same switches | 475-476 MB | **0.52x** |
| max on chrome-headless-shell | | 467-468 MB | 0.51x |

Workload B (real sites): stock 838, balanced 624 (0.74x), max 606 (0.72x), hs-balanced 540-559 (0.64-0.67x).
All levels: 0.0% screenshot difference vs stock on the ELF (incl. WebGL); smoke PASS incl. the
56-tab background-eval check. Through cu (`CU_BROWSER=slimium-chrome`, cu's own argv merged):
650-698 MB vs 779-790 for cu's own flags (0.82-0.88x of cu, 0.71-0.76x of stock).

Flags alone top out at ~0.70x on the Fedora ELF for content-heavy pages: after the
process model is collapsed, ~75% of what is left is the pages' own live data in
the one renderer (V8 + Blink heaps) plus ~260 MB of resident engine code and data
files. The engine binary is the next lever (headless shell: browser process anon 18
vs 62 MB, file union ~150 vs ~260 MB), then cgroup file-page reclaim (gov track,
see docs/RESEARCH-structural.md).

## balanced (and max) switches

| switch | why | measured (workload A unless noted) |
|---|---|---|
| `--renderer-process-limit=1` | all tabs and frames share one renderer; the per-renderer base cost (~10-25 MB each, plus its own copy of V8/Blink caches) is paid once | process-model group: **-140 MB** PSS (21 -> 8 procs); the largest single saving. The profile **overrides** a caller's value (cu passes 4) |
| `--disable-site-isolation-trials` + `--disable-features=IsolateOrigins,site-per-process` | without them cross-site frames get their own (OOP) renderer regardless of the limit | part of the group above; security: a compromised renderer can read other sites' data (same as Android low-memory devices) |
| `--disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup,WebUIOmniboxFullPopup` | headless=new 156 preloads omnibox-popup WebUI renderers nobody can see | with `--disable-component-extensions-with-background-pages`: **-45 MB** PSS, -2 procs; idle tree -56 MB (structural research) |
| `--disable-component-extensions-with-background-pages` | the hangouts component extension keeps a background page alive even with `--disable-extensions` | in the group above |
| `--enable-features=NetworkServiceInProcess2` | network service as a browser thread, not a utility process (156's name; `NetworkServiceInProcess`, which cu passes, does nothing on 156) | **-55 MB** PSS, -1 proc |
| `--disable-extensions`, `--disable-background-networking`, `--disable-component-update`, `--disable-default-apps`, `--disable-sync`, `--disable-breakpad` | Fedora's chromium installs an external extension (GNOME connector) that spawns an extension renderer plus a python3 native host (16 MB) | ~-30 MB PSS (noisy); stops background traffic |
| `--disable-features=Translate,OptimizationHints,OptimizationGuideModelDownloading,OptimizationHintsFetching,OptimizationTargetPrediction,MediaRouter,DialMediaRouteProvider,AutofillServerCommunication,CertificateTransparencyComponentUpdater,InterestFeedContentSuggestions,CalculateNativeWinOcclusion,HeavyAdIntervention,LensOverlay` | background services and model downloads | ~-30 to -40 MB PSS together (noisy; structural research found ~-2 MB for ~40 browser features on an idle-ish tree — kept for the traffic) |
| `--disable-features=BackForwardCache,SpareRendererForSitePerProcess` | no cached pages / spare renderers kept alive | ~0 on A (single navigation per tab); saves a renderer per navigated tab in long sessions |
| `--enable-features=PartitionAllocMemoryReclaimer,PartitionAllocUseDenserDistribution,PartitionAllocSortActiveSlotSpans,PartitionAllocStraightenLargerSlotSpanFreeLists,PartitionAllocSortSmallerSlotSpanFreeLists,MemoryPurgeInBackground,MemoryPurgeOnFreeze` | allocator packing and reclaim; no security cost | a few MB (research, combined with the hardening knobs: -8 MB) |
| `--js-flags=--max-semi-space-size=1` (balanced), `... --lite-mode` (max only) | 1 MB young generation per isolate; lite mode: no optimizing JIT, lazy feedback vectors, smaller V8 heaps | **-120 MB iso** on A (balanced without it: 766-775, with it: 643-647); caveat: CPU-bound JS ~2.5-4x slower (`bench/www/jsperf.html`: 142-226 ms stock vs 555-647 ms), page loads unchanged in the benchmark |
| `--no-first-run` | no first-run UI | - |

max adds:

| switch | why | caveat |
|---|---|---|
| `--disable-features=PartitionAllocBackupRefPtr,PartitionAllocSchedulerLoopQuarantine,PartitionAllocLargeThreadCacheSize,PartitionAllocEventuallyZeroFreedMemory,GwpAsanMalloc,GwpAsanPartitionAlloc,PartitionAllocLargeEmptySlotSpanRing,PartitionAllocAdjustSizeWhenInForeground` | removes per-allocation hardening overhead and quarantined frees | -8 MB (headless shell 476 -> 468; research -8 on the cu set); **security**: weakens use-after-free mitigation |

## Rejected (measured)

| candidate | result |
|---|---|
| `--disable-gpu`, `--in-process-gpu`, `--disable-software-rasterizer`, `--disable-gpu-compositing`, `--use-gl=disabled`, `--use-angle=disabled`, Vulkan/SkiaGraphite off | the GPU process stays (only `--in-process-gpu` removes it); `--in-process-gpu --disable-gpu` was **+25 MB** PSS vs keeping the GPU process; iso: `--disable-gpu` ±0, `--in-process-gpu` +20..+80. `--disable-gpu` also loses WebGL (fidelity). cu's own `--disable-gpu` is left alone: removing it changed nothing (654 vs 653 MB) |
| `--enable-low-end-device-mode` | -21 MB on the cu set (research; mostly because it turns on V8 lite mode) but it **freezes background tabs**: with workload A + 50 tabs open, `Runtime.evaluate` on the 8 oldest tabs hangs (20 s timeout). cu evaluates in background tabs — **rejected**; the smoke check now tests this permanently |
| `--single-process` | -150 MB PSS but breaks `Target.createBrowserContext` (the browser dies; cu track: 53/71) — **forbidden** |
| `--no-zygote` (+`--no-sandbox`) | **+200 MB** PSS (962 vs 750): every renderer is exec'd fresh and loses the zygote's copy-on-write base |
| `--no-sandbox` | ±0 |
| `--process-per-site` | ±0 once the renderer limit is 1 |
| `--js-flags=--max-lazy / --optimize-for-size / --jitless / --single-threaded-gc / --no-expose-wasm` | within noise on A; `--optimize-for-size` ≈ `--lite-mode` on the cu set |
| `--disk-cache-size`, `--aggressive-cache-discard`, `--disable-dev-shm-usage`, `--force-device-scale-factor=1`, `--window-size=800,600`, `--num-raster-threads=1`, `--disable-renderer-accessibility`, `--enable-aggressive-domstorage-flushing`, `--force-gpu-mem-available-mb`, `--memory-model=low` | no measurable effect (the last one is not a Chromium switch) |
| `--blink-settings=imagesEnabled=false` | changes rendering — out (hard requirement: screenshots like stock) |

## Merge semantics (`slim_profile_merge_argv`)

The wrapper keeps the caller's argv (cu's) and adds the profile:

- Result order: engine, caller switches (in order), profile switches, then the caller's positional
  args (URLs, `about:blank`, everything after a bare `--`).
- **Profile wins** for memory switches: `--renderer-process-limit`, `--process-per-site`,
  `--process-per-tab`, `--single-process`, `--no-zygote`, `--in-process-gpu`, `--disable-gpu*`,
  `--disable-software-rasterizer`, `--disable-site-isolation-trials` (also drops a caller's
  `--site-per-process` / `--isolate-origins`), `--enable-low-end-device-mode`,
  `--num-raster-threads`, `--force-device-scale-factor`, `--disk-cache-size`, `--aggressive-cache-discard`.
- **Caller wins** for everything else (identity and function: `--user-data-dir`, `--remote-debugging-port`,
  `--headless`, `--user-agent`, `--window-size`, `--proxy-server`, `--lang`, ...). A switch is never duplicated.
- `--enable-features`, `--disable-features`, `--enable-blink-features`, `--disable-blink-features`
  (comma lists) and `--js-flags` (space list) become **one switch each** — Chromium honours only the last
  occurrence — holding every caller item (all occurrences) plus the profile's. A profile feature the caller
  put in the opposite list is removed from the caller's list (profile wins); a profile V8 flag replaces the
  caller's flag of the same name (`--no-` and `_` spellings included).
- A caller asking for GL (`--use-gl`, `--use-angle`, `--enable-unsafe-swiftshader`, `--enable-gpu`) never gets
  `--disable-gpu`, `--disable-gpu-compositing`, `--in-process-gpu` or `--disable-software-rasterizer`.
- One allocation: `free()` the result once. Tests: `bench/test_profile.c` (`make -C bench test`).

## Engine notes

- `/usr/bin/chromium-browser` is a bash wrapper that sources `/etc/chromium/chromium.conf`, adds its own
  `--disable-features`/`--enable-features` (overridden by any later one) and keeps two `cat` processes;
  slimium execs the ELF directly.
- chrome-headless-shell (CfT 156.0.8078.4): stock 636 MB iso vs 910 for the ELF; with balanced 476 (0.52x).
  Screenshots: 0.0% diff on images, canvas, SVG, CSS, WebGL, full-page and en.wikipedia.org; **0.6% of pixels
  differ on the text fixture** — glyph rasterisation of CFF/OpenType fonts (the web font and the monospace
  font) differs from Fedora's build (bundled vs system FreeType). TrueType/system sans text is identical.
  56 tabs: heavier than the ELF with balanced (1330 vs 1193 MB). Its UA carries the full version
  (`HeadlessChrome/156.0.8078.4`).
