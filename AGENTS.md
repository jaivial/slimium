# slimium — agent rules

slimium is a Chromium-based browser for agents: a tiny C11 launcher/driver that runs the
system Chromium engine under a low-memory process model and exposes every operation as a CLI
command over the DevTools protocol. Target: <= 50% of stock Chromium's RAM (PSS of the whole
process tree) on the same workload, and drop-in use as cu's browser (`CU_BROWSER=slimium-chrome`).

## Hard rules
- Language: C11 only (lowest RAM). libc + POSIX only; no third-party libraries, no C++.
  Small vendored code is allowed only if written in C and license-compatible (prefer own code).
- Build: `make` (gcc, `-Os`), `make test`, `make bench`. Warnings are errors (`-Wall -Wextra -Werror`).
- The slimium binary must stay small (< 300 KB stripped) and its own RSS < 2 MB per command.
- Never touch the user's running cu daemon (port 8787, CDP 9222, ~/.cache/cu or its data dir) nor
  ~/cu's main checkout. cu work happens in a separate worktree and a separately deployed `cu-slim`.
- Never kill processes you did not start. Use unique CDP ports (9300-9399 for tests) and temp profiles.
- Output for agents: compact, one JSON object per line with `--json`; plain text otherwise.
  Exit code 0 on success, non-zero with a one-line error on stderr.

## Layout
- `src/profile.c|h`  low-memory Chromium flag profiles (levels: off, balanced, max). Owner: memory track.
- `src/mem.c|h`      PSS/RSS accounting of a process tree via /proc/<pid>/smaps_rollup. Owner: memory track.
- `src/ws.c|h`, `src/json.c|h`, `src/b64.c|h`, `src/cdp.c|h`  DevTools client. Owner: core track.
- `src/main.c`, `src/cmd_*.c`  CLI. Owner: core track.
- `bench/` RAM benchmark (stock vs slimium). `tests/` functional tests (shell + C), `tests/cu/` cu-slim tests.
