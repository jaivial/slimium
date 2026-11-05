# Structural memory levers: research notes (Chromium 156, cu-like 13-tab workload)

Author: structural-research agent, 2026-11-05. Not committed. Raw data is in /tmp/slimres (results*.jsonl, fid/*.png, harness h.py).

## 0. Method and metric (read this first)

- Workload: 13 tabs opened with `PUT /json/new?URL`: 10 local fixture pages (http://127.0.0.1:9429/pN.html: a form,
  SVG, canvas, 120 cards, a 100x8 table, a 3000-object JS array), example.com, en.wikipedia.org/wiki/Chromium_(web_browser),
  news.ycombinator.com. `--headless=new`, settle 20 s, then measure. Base flags = cu's `FAST_TEST_ARGS` (~/cu/src/policy.rs)
  plus cu's `--user-agent`, unless the row says "stock" (no flags except port, profile, headless=new).
- **Raw PSS is not a usable metric on this machine.** PSS of a file page is divided among all processes that map it,
  system-wide. Around 60 other Chromium processes (other agents, the user's cu) map the same
  /usr/lib64/chromium-browser/chromium-browser, so a tree's Pss_File is deflated and swings 49 to 80 MB between
  identical runs. A different binary (Google Chrome, headless_shell) looks much worse on raw PSS only because no
  one else shares its pages.
- **Metric used here ("iso", the isolated footprint)** = sum(Pss_Anon) + sum(Pss_Shmem) over the tree + the UNION of
  resident file-backed pages mapped by any process in the tree (from /proc/PID/pagemap present bits, keyed by
  (file, page offset)). In practice it equals the raw PSS you would see if this browser were the only one running.
  Check: with a private copy of the ELF, raw PSS was 412-425 for iso 485-488, and the gap is the system libraries
  (libc, glib, ...) that the rest of the desktop also maps. Pss_Anon is stable to within ±2 MB between runs; iso is
  stable to within ±5 MB.
- Stock iso = **731-735 MB** (25 processes), so the 0.5x target is **≤ 367 MB**.
- Every candidate also passed: the JS on a fixture page ran, Page.captureScreenshot worked, and
  Target.createBrowserContext plus a tab in that context ran JS (ctx=1).
- Screenshot fidelity: fixtures/fidelity.html has a web font (@font-face), PNG and SVG images, a video poster,
  canvas 2D, a WebGL triangle, inline SVG, CSS gradients, transforms, shadows, and a tall body for the full-page shot.
  Shots are 800x600 at DPR 1: viewport, full page 800x1500 (captureBeyondViewport), an element clip, and the
  wikipedia tab. Each is compared with stock (fail if more than 1% of pixels have a channel diff > 16). The WebGL
  triangle renders in stock and in every profile (pixel counts checked), because --disable-gpu still gives
  SwiftShader WebGL in the GPU process.

## 1. Where the memory is (L = limit=1 + NSIP2 + WebUI/component-ext fix, 13 tabs)

| process (n) | Pss_Anon MB | notes |
|---|---|---|
| browser (1) | 69 | memory-infra: malloc 51 MB (48 MB live objects), sqlite 1, cc 1. About 65 MB of this exists with a single blank tab, so it is a fixed cost |
| renderer (1) | 115 | blink_gc 41, malloc 37 (blink_objects 31), partition_alloc 11, v8 10, web_cache 3, cc tiles 2.5 |
| zygotes (3) | 21.5 | COW base for the children; --no-zygote / --no-sandbox do not help |
| gpu-process (1) | 16.5 | SwiftShader/viz; malloc 14 |
| storage util (1) | 8 | |
| **file union** | **281** | **chromium-browser ELF 210 MB resident**, then CJK fonts 4.7, icudtl 2.7, libcrypto 2.6, and about 60 small system libraries (~40 MB total) |

Key finding: **the biggest single item is the resident ELF code (210 MB, 40% of iso)**. No runtime flag changed it
by more than ±5 MB. It only moves with a different binary or with page reclaim (§3).

**Bug-level finding (already sent):** with limit=1, headless=new Chromium 156 still starts 2 extra WebUI renderers
(chrome://omnibox-popup.top-chrome/ and .../omnibox_popup_aim.html, type browser_ui) plus the hangouts component
extension (nkeimhogjdpnpccoofpliimaahmaaome, Fedora builds with enable_hangout_services_extension=true;
--disable-extensions does not remove it). Fix: `--disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup` and
`--disable-component-extensions-with-background-pages`. In Chromium 156 most feature names exist in the ELF only as the
`kName` string (BASE_FEATURE derives the name by dropping the leading k), so grep `^k[A-Z]` in `strings` output.

## 2. Lever table (iso MB; stock = 733; fidelity = pixel diff vs stock on viewport/full/clip/wiki)

| # | lever (added on top of the row it says) | iso MB | x stock | Δ iso | Pss_Anon | fidelity | breaks? |
|---|---|---|---|---|---|---|---|
| S | stock, no flags | 731-735 | 1.00 | | 414 | ref | |
| C | cu FAST_TEST_ARGS (limit=4) | 590 | 0.80 | | 285-293 | 0% | |
| 1 | C + --renderer-process-limit=1 + NetworkServiceInProcess2 | ~555 (anon-derived) | 0.76 | -35 | 262 | 0% | no (one renderer crash takes all tabs) |
| 2 | **L** = 1 + WebUIOmniboxPopup,WebUIOmniboxAimPopup off + --disable-component-extensions-with-background-pages | 519-520 | 0.71 | -35 | 230 | 0% | no |
| 3 | L + --enable-low-end-device-mode | 499 | 0.68 | -21 | 208 | 0% | no |
| 4 | L + --js-flags=--lite-mode (or --optimize-for-size, same effect) | 493 | 0.67 | -26 | 207 | 0% | JS slower (no optimizing JIT); -25 MB renderer V8 |
| 5 | L + PartitionAlloc knobs (on: PartitionAllocMemoryReclaimer, UseDenserDistribution, SortActiveSlotSpans, StraightenLargerSlotSpanFreeLists, SortSmallerSlotSpanFreeLists, MemoryPurgeInBackground, MemoryPurgeOnFreeze; off: PartitionAllocBackupRefPtr, SchedulerLoopQuarantine, LargeThreadCacheSize, EventuallyZeroFreedMemory, GwpAsanMalloc, GwpAsanPartitionAlloc, LargeEmptySlotSpanRing, AdjustSizeWhenInForeground) | 511 | 0.70 | -8 | 223 | 0% | turning BRP and the quarantine off removes UAF hardening |
| 6 | **BEST** = L + 3 + 5 + --js-flags=--max-semi-space-size=1 | 487-488 | 0.66 | -32 | 197 | 0% | no |
| 7 | L + governor (browser + each page: Memory.simulatePressureNotification critical, HeapProfiler.collectGarbage), measured 30 s later | 520 → 497 | 0.68 | -23 | 230 → 204 | 0% | no |
| 7b | same + Page.setWebLifecycleState frozen on background tabs | same as 7 | | 0 | | 0% | frozen tabs stop timers; no extra saving, so don't freeze |
| 7c | BEST + governor | 488 → 488 | 0.66 | 0 | | 0% | low-end mode already GCs aggressively |
| 8 | BEST + **hibernate** background tabs (keep the 3 most recent; Page.navigate about:blank, restore later with Page.navigateToHistoryEntry) + governor | 461 | 0.63 | -27 | 164 | 0% | restore tested: JS state reloads, form value + scroll restored ([1,"hib-state",700]) |
| 9 | **cgroup file-page reclaim** (memory.reclaim "400M swappiness=0" on every cgroup of the tree), on BEST + 8 | 293 right after, 318 steady with a reclaim every 30 s, 359 just before the next reclaim, 482 during active use → 318 | **0.40-0.49** | -150 to -190 idle | 180-193 (no swap used) | 0% | no; screenshot latency after reclaim 15-31 ms vs 29-81 ms warm; ~640 ms of kernel time per reclaim |
| E1 | engine: Google Chrome 156.0.8078.47 (official PGO+ThinLTO, /opt/google/chrome/chrome), L | 467-473 | 0.64 | -50 vs Fedora L | 218-220 | 0% | proprietary binary; adds liboptimization_guide_internal.so etc. |
| E2 | Google Chrome + BEST | 437 | 0.60 | | 186 | 0% | |
| E3 | Google Chrome + BEST + hibernate + governor | 404 | 0.55 | | 151 | 0% | |
| E4 | Fedora chromium-headless 156.0.8078.79 headless_shell (rpm extracted, no root), L | 493 | 0.67 | -26 | 243 (gpu-proc 40, shm 33) | **FAIL**: viewport 4.5%, full 7.5% (text antialiasing differs), wiki 0.02% | |
| E5 | chrome-headless-shell 156.0.8078.4 (Chrome for Testing, Playwright cache), L | 420 | 0.57 | -100 | 228 (browser 43) | not measured; probably the same text-AA difference as E4 | earlier "537 MB, worse" was a raw-PSS sharing artifact |
| x | --in-process-gpu | 559 | | **+39** | 233 | 0% | reject: browser +10, zygote +10, more code mapped |
| x | GPU-process killers: --disable-software-rasterizer, --disable-gpu-compositing, --use-gl=disabled, --use-angle=disabled, --ozone-platform=headless, --disable-features=Vulkan,SkiaGraphite (each and all together) | | | 0 to +5 anon | 231-237 | 0% | the GPU process never goes away; several variants add a "broker" process (+7-10 MB) |
| x | V8 --single-threaded-gc, --no-opt --max-lazy, --max-old-space-size=128 | | | 0 | 230 | 0% | no effect on this workload |
| x | --process-per-site (with limit=1) | | | 0 | 231 | 0% | |
| x | ~40 browser features off (OptimizationGuide*, Reporting, Fledge/Topics/PrivacySandbox, Compose, Glic, LensOverlay, ShoppingList, Prerender2, LoadingPredictorPrefetch, NetworkTimeServiceQuerying, DeviceBoundSessions, ...) + --disable-client-side-phishing-detection, --disable-domain-reliability, ... | | | -2 | 230 | 0% | not worth the list |
| x | --disable-field-trial-config | | | 0 | 231 | 0% | Fedora already builds with disable_fieldtrial_testing_config=true |
| x | --no-sandbox | 488 | | 0 | 197 | 0% | reject (security, no gain) |
| x | --blink-settings=imagesEnabled=false | | | -5 anon | 227 | images missing (wiki 0/18 loaded) | rejected by the rendering requirement; possible opt-in "lean" mode at most |

## 3. The lever that reaches 0.5x: cgroup proactive reclaim of file pages

About 210 MB of the iso footprint is ELF code that was touched once (start-up, first navigation) and is cold after
that. cgroup v2 `memory.reclaim` (kernel ≥ 5.19; `swappiness=` argument ≥ 6.x; this host runs 7.2) evicts cold pages
that are charged to a cgroup. It is writable by the user inside user@UID.service, so no root is needed. With
`swappiness=0` only file pages go (swap stayed at 0 in every run), so the RAM really is freed: it is not swap and not zram.
The pages that are needed again fault back in from disk.

Measured (BEST + hibernate + governor, Fedora ELF): file union 288 → 103 MB right after the reclaim, then 139 after
2 screenshots, 172 after 30 s idle, and 131 after another reclaim plus 30 s. After a full active-use pass (restore a
hibernated tab, JS checks, opening the fidelity page in a new tab, 4 screenshots) it was back at 272, and the next
reclaim brought it to 109.
**Iso: 488 (settle) → 293 / 318 (steady, reclaiming every 30 s) / 359 (just before the next reclaim) / 482 (peak during use).**
Screenshots stayed pixel-identical.

Implementation notes for slimium (launcher/daemon, C, no root):
1. Start the engine inside its own scope, e.g. `systemd-run --user --scope -q --unit=slimium-PORT ENGINE ...` (or mkdir a
   cgroup under a delegated subtree). Chromium's browser process then **moves itself** into
   `app-org.chromium.Chromium-PID.scope`, while the zygote and its children stay in ours. So collect the cgroup of every
   pid in the tree (/proc/PID/cgroup) and reclaim each one.
2. Pages are charged to the cgroup that first faulted them into the page cache. If the ELF is already cached and
   charged to another cgroup (another Chromium, or a previous slimium whose cgroup died: its pages get reparented),
   our reclaim cannot evict them. Mitigation: before launch, `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` on the engine
   ELF drops its unmapped cached pages, which works on root-owned read-only files. In the experiment I used a private
   copy in ~/.cache with fadvise to guarantee this. With the shared system ELF it works when no other Chromium is running,
   which is the normal cu case.
3. Policy: reclaim once after the first settle, then every 15-30 s while no CDP command has arrived for more than 5 s.
   Write `"<N>M swappiness=0"`; EAGAIN just means nothing more was reclaimable. Cost is ~0.6 s of kernel CPU per pass,
   and no screenshot slowdown was measurable.
4. The bench must report both "settled after governor" and "peak during an active pass" so this lever is not hidden.

## 4. Custom build feasibility (A)

- Disk: /home has 153 GB free. A no-history depot_tools checkout at tag 156.0.8078.79 is ~35-40 GB plus ~10-15 GB
  for an official symbol_level=0 out dir. The Fedora SRPM route (`dnf download --source chromium`, clean tarball,
  `rpmbuild` with edited `CHROMIUM_*_GN_DEFINES`) needs ~25-35 GB and no depot_tools. Either fits.
- depot_tools route: git clone depot_tools, `fetch --no-history chromium`, `git checkout tags/156.0.8078.79`,
  `gclient sync -D --with_branch_heads` with `checkout_pgo_profiles=True` (needed for PGO). It uses the bundled clang/rust and
  the Debian sysroot, so install-build-deps.sh, which does not support Fedora, is not needed.
- Time on 16 cores / 59 GB without remote execution: about 4-6 h for a clean `chrome` target (~60-70k steps). ThinLTO+PGO
  adds ~1 h of link time and needs ~25-35 GB RAM peak, which fits but is tight with zram. Chromium ships a new major
  roughly every 4 weeks plus security respins, so this is a recurring cost.
- Which GN args change RUNTIME memory: Fedora already sets is_official_build=true, symbol_level=0,
  blink_symbol_level=0, disable_fieldtrial_testing_config=true, enable_vr=false, and NaCl is gone in 156. Symbols are
  never resident anyway, so those give **0**. What is left:
  - chrome_pgo_phase=2 + use_thin_lto=true (+ the lld call-graph ordering Google uses): measured directly with Google
    Chrome 156.0.8078.47 (same milestone, same flags) = **-41 MB file union, -11 MB anon, -50 iso (7% of stock)**.
  - enable_pdf/printing/spellcheck/reporting/safe_browsing off, enable_hangout_services_extension=false: code that is
    never executed is never resident, and the runtime feature disables already measured ≤ 2 MB. Expect ≤ 5-10 MB.
    enable_extensions=false is not a supported desktop configuration.
  - Build headless_shell instead of chrome (ozone headless only, no GTK/X): Fedora headless_shell = -26 iso, PGO'd CfT
    headless shell = -100 iso. But text renders measurably differently (4.5-7.5% of pixels on the font fixture), which fails
    the "looks like stock Chromium" requirement unless that difference is accepted.
  - use_partition_alloc tuning: same as the runtime PA feature knobs (row 5, -8). No extra gain from GN.
- **Verdict: not worth it.** The best realistic custom build (PGO+ThinLTO, chrome target) is worth ~50 MB (~0.07x).
  The same saving is available today by pointing slimium at Google Chrome 156 if it is installed (row E1, 0% fidelity
  diff). Neither reaches 0.5x on its own; the reclaim governor does.

## 5. Recommendation (through cu)

Runtime set that reaches ≤ 0.5x on the iso metric with 0% screenshot diff and working createBrowserContext:

1. Flags (profile "max"): cu FAST_TEST_ARGS + `--renderer-process-limit=1`, `--enable-features=NetworkServiceInProcess2`,
   `--disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup,WebUIOmniboxFullPopup`,
   `--disable-component-extensions-with-background-pages`, `--enable-low-end-device-mode`, `--js-flags=--max-semi-space-size=1`,
   plus the PA feature knobs of row 5 (optional; -8 MB, weakens UAF hardening). Merge every --enable-features /
   --disable-features / --js-flags into ONE switch each, because Chromium keeps only the last occurrence.
   → **iso 488 (0.66x)**.
2. Governor after settle: Memory.simulatePressureNotification critical on the browser + HeapProfiler.collectGarbage per
   page (-23 on L, ~0 on BEST). Do not freeze, and never use Memory.forciblyPurgeJavaScriptMemory.
3. Optional tab hibernation of background tabs beyond the N most recent (about:blank + navigateToHistoryEntry on
   use; form state and scroll survive, in-memory JS state does not) → **461 (0.63x)**. Make it opt-in or apply it only
   to tabs untouched for more than X minutes.
4. **cgroup file-page reclaim** (§3) → **293-318 steady (0.40-0.43x), ≤ 359 between passes (0.49x)**, peak ~480 during active use.
5. If /opt/google/chrome/chrome 156 exists, it is a further -50 MB everywhere (E3 + reclaim should give ~0.35-0.4x),
   but it is a proprietary engine, so keep it an opt-in engine choice.

Without step 4, no runtime lever I found gets below ~0.55x (best: Google Chrome + BEST + hibernate = 404). The
remaining floor is the ~65 MB fixed browser heap, ~85 MB renderer for 13 real pages, ~21 MB zygotes, ~16 MB GPU
process, ~8 MB storage, and 240-280 MB of resident code.

## 6. Pagemap-union routine (C, for src/mem.c)

```c
/* Union of resident file-backed pages over a set of pids, in kB. Unprivileged: for same-uid processes
 * /proc/PID/pagemap gives flags (bit 63 = present) with PFNs zeroed; PFNs are not needed because a page is
 * identified by (st_dev, st_ino, file page offset) from /proc/PID/maps.
 * iso_kb = sum(Pss_Anon) + sum(Pss_Shmem) [smaps_rollup] + slim_file_union_kb(...)
 * Skip ino==0 (anon/heap/stack/vdso), "/memfd:", "(deleted)", "/dev/*" and files on tmpfs (already in Pss_Shmem). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

typedef struct { unsigned maj, min; unsigned long ino; uint8_t *bits; size_t nbytes; } ufile;

static ufile *uf_get(ufile **v, size_t *n, size_t *cap, unsigned maj, unsigned min, unsigned long ino) {
    for (size_t i = 0; i < *n; i++) if ((*v)[i].ino == ino && (*v)[i].maj == maj && (*v)[i].min == min) return &(*v)[i];
    if (*n == *cap) { size_t c = *cap ? *cap * 2 : 64; ufile *t = realloc(*v, c * sizeof *t); if (!t) return NULL; *v = t; *cap = c; }
    ufile *f = &(*v)[(*n)++]; f->maj = maj; f->min = min; f->ino = ino; f->bits = NULL; f->nbytes = 0; return f;
}

static long uf_mark(ufile *f, uint64_t pg) {                 /* returns 1 if newly set */
    size_t byte = (size_t)(pg >> 3);
    if (byte >= f->nbytes) {
        size_t nb = (byte + 1) * 2; uint8_t *t = realloc(f->bits, nb); if (!t) return 0;
        memset(t + f->nbytes, 0, nb - f->nbytes); f->bits = t; f->nbytes = nb;
    }
    uint8_t m = (uint8_t)(1u << (pg & 7));
    if (f->bits[byte] & m) return 0;
    f->bits[byte] |= m; return 1;
}

long slim_file_union_kb(const pid_t *pids, int npids) {
    ufile *v = NULL; size_t n = 0, cap = 0; long pages = 0; char path[64], line[4096]; uint64_t buf[512];
    long psz = sysconf(_SC_PAGESIZE);
    for (int k = 0; k < npids; k++) {
        snprintf(path, sizeof path, "/proc/%d/maps", (int)pids[k]); FILE *m = fopen(path, "r"); if (!m) continue;
        snprintf(path, sizeof path, "/proc/%d/pagemap", (int)pids[k]); int pm = open(path, O_RDONLY);
        if (pm < 0) { fclose(m); continue; }
        while (fgets(line, sizeof line, m)) {
            unsigned long a, b, off, ino; unsigned maj, min; char perm[8]; int pos = 0;
            if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %n", &a, &b, perm, &off, &maj, &min, &ino, &pos) < 7 || !ino) continue;
            const char *p = line + pos;
            if (*p != '/' || !strncmp(p, "/memfd:", 7) || !strncmp(p, "/dev/", 5) || strstr(p, "(deleted)")) continue;
            ufile *f = uf_get(&v, &n, &cap, maj, min, ino); if (!f) continue;
            uint64_t first = off / (unsigned long)psz;
            for (unsigned long va = a; va < b; ) {
                size_t cnt = (b - va) / (unsigned long)psz; if (cnt > 512) cnt = 512;
                ssize_t r = pread(pm, buf, cnt * 8, (off_t)(va / (unsigned long)psz) * 8);
                if (r <= 0) break;
                for (size_t i = 0; i < (size_t)r / 8; i++)
                    if (buf[i] >> 63 & 1) pages += uf_mark(f, first + (va - a) / (unsigned long)psz + i);
                va += cnt * (unsigned long)psz;
            }
        }
        close(pm); fclose(m);
    }
    for (size_t i = 0; i < n; i++) free(v[i].bits);
    free(v);
    return pages * (psz / 1024);
}
```
Memory: the ELF needs a ~10 KB bitmap, and all files together stay well under 1 MB, so the < 2 MB RSS rule holds.
The tmpfs check (statfs f_type == TMPFS_MAGIC on the path) is omitted above for brevity. Add it if engines may
live in /tmp.
