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
- Never touch the user's running cu daemon. It normally uses HTTP 8810 and CDP 9362 (and may walk
  through 8834/9386); currently another session may own 8810/9363 while systemd retries. Never use
  8810-8834 or 9362-9386, never touch ~/.cu, and never signal its processes. Do not edit ~/cu's main
  checkout. cu work happens in a separate worktree and the isolated `cu-slim` (8790-8799/9390-9391).
- Never kill processes you did not start. Each track uses only its assigned CDP ports and temp profiles.
- New command modules export `const cmd_def *slim_<track>_commands(size_t *count)`; weak module hooks
  live in src/cmd_modules.c. Do not edit main.c for ordinary command registration. In-process callers
  use cli_invoke() (no stdout emission) and inspect ctx_t.oj/ot.
- Named user profiles are distinct from memory profiles: `--profile` remains off|balanced|max; use
  `--user NAME` / SLIM_USER_PROFILE for a persistent website-login profile.
- Output for agents: compact, one JSON object per line with `--json`; plain text otherwise.
  Exit code 0 on success, non-zero with a one-line error on stderr.

## Layout
- `src/profile.c|h`  low-memory Chromium flag profiles (levels: off, balanced, max). Owner: memory track.
- `src/mem.c|h`      PSS/RSS accounting of a process tree via /proc/<pid>/smaps_rollup. Owner: memory track.
- `src/ws.c|h`, `src/json.c|h`, `src/b64.c|h`, `src/cdp.c|h`  DevTools client. Owner: core track.
- `src/main.c`, `src/cmd_*.c`  CLI. Owner: core track.
- `bench/` RAM benchmark (stock vs slimium). `tests/` functional tests (shell + C), `tests/cu/` cu-slim tests.
