# Media: recording and responsive capture

All media commands use the existing session/current tab. They do not navigate or reload the page.
Recording has **no audio stream**.

## Record a page

~~~sh
slimium record start page.mp4
slimium record start page.webm --fps 15 --quality 75 --max-seconds 30
slimium --tab TARGET record start page.avi --format avi
slimium record status                 # active recordings in this session
slimium --json record stop            # one active recording
slimium record stop --id r...         # explicit when several are active
slimium record list                   # active and completed recordings
~~~

Options:

- `--fps N`: constant output rate, 1–60 (default 10). The latest screencast image is repeated
  between Chromium's change-driven events, so a static page still produces a constant-FPS video.
- `--format mp4|webm|avi`: container/encoder; inferred from the output extension, default MP4.
  MP4 uses H.264, WebM uses VP9, and AVI uses MPEG-4.
- `--quality N`: 0–100 (default 75), applied to screencast JPEG and the output encoder.
- `--max-seconds N`: automatically stop and finalize after 1–86400 seconds.
- `--tab ID`: existing global tab selector; use a full target id or unique prefix.

`record start` returns only after the worker has its own CDP connection, received its first frame,
and started the encoder. Output JSON includes the unique id, worker pid, path, format, FPS, canvas,
and audio:false. Stop/status/list include source frame count, duplicated output frame count,
duration, and finalized byte count when available.

### Lifecycle and safety

The worker is a detached instance of the same slimium binary running the internal
`__record-worker` command. It uses `Page.startScreencast` and immediately acknowledges each
`Page.screencastFrame` with `Page.screencastFrameAck`. It requests JPEG; the frame parser and
image2pipe input also accept PNG dimensions/data.

`/usr/bin/ffmpeg` (or ffmpeg in PATH) is an **optional runtime executable**, not a build or link
dependency. Recording reports a compact error if it is unavailable. The encoder is a child of the
recording worker, never of an unrelated browser/CLI process.

State and logs are `record-<session>-<id>.json[.log]` in the existing 0700 session runtime
directory; atomic metadata files are 0600. `--port` mode uses an isolated `port-N` metadata scope.
The final video is written to `FILE.<id>.part.<format>` and atomically renamed to FILE only after
ffmpeg exits successfully. SIGTERM/SIGINT, max-seconds, target detach, and browser disconnect all
close the encoder input and wait for finalization. Browser-death recordings therefore remain
playable; list metadata includes a short informational error such as "target detached; recording
finalized". Startup/encoder failures never leave a recorder or ffmpeg process running.

`record stop` signals only the exact worker pid after checking its `/proc` start time, executable
identity, internal command line, and recording id. The worker waits for its own ffmpeg child and
uses bounded TERM/KILL only if that child fails to exit. No browser process is signaled by record
commands.

### Recording through viewport changes

The video canvas is fixed from the first frame (rounded up to even dimensions for YUV420). Later
frames are aspect-fitted and black-padded to that canvas. This lets the encoder survive live
viewport changes without reinitializing the stream. The worker watches the saved session viewport
and reapplies changes on its own CDP session. Screenshot and regular CLI commands remain usable
while recording.

## Live viewport and responsive screenshots

~~~sh
slimium viewport 800 600
slimium resize 900 500                 # alias
slimium viewport 390 844 --scale 2 --mobile
slimium viewport mobile                # 375x812
slimium viewport tablet                # 768x1024
slimium viewport desktop               # 1440x900
slimium viewport reset
slimium responsive shots --sizes 375x812,768x1024,1440x900
~~~

Named presets are exact CSS-pixel dimensions and do not implicitly enable mobile emulation;
`--mobile` remains an explicit option. Existing saved-session behavior is retained: every new
slimium attach reapplies the metrics override, updating innerWidth/innerHeight and CSS media
queries without navigation. Screenshots and recording see the new size immediately.

`responsive` applies each size to the already-open page, waits two animation frames, verifies the
exact DOM viewport, writes `OUTDIR/WxH.png`, and restores the prior override (or native viewport)
before returning. It restores on errors as well. It does not change the saved viewport or reload
the page. In stateless `--port` mode, restoration means clearing the override because there is no
saved session viewport.

## Tests

~~~sh
make
tests/media.sh                       # isolated CDP 9490, fresh /tmp state/profile
SLIM_TEST_PORT=9491 tests/media.sh   # allowed media test range: 9490-9509
SLIM_MEDIA_KEEP=1 tests/media.sh     # retain videos, screenshots, first/last frames
~~~

The suite checks exact PNG dimensions and DOM breakpoints; PNG/JPEG/WebP screenshots; recording
across independent CLI commands and a resize; MP4/WebM/AVI ffprobe streams and durations; static
frame duplication; decoded first/last pixel differences; max-seconds; failed-start cleanup;
browser-death finalization; and absence of recorded worker/ffmpeg processes. Per-command timing
and peak RSS are written to `/tmp/slimium-media-metrics.tsv`.

Integration: `src/*.c` wildcard builds the new module automatically. The exported registry is
`slim_media_commands(size_t *)`; the existing weak hook discovers it. The orchestrator should add
`tests/media.sh` to the aggregate test target. No Makefile/main/cmd_modules changes are needed.
