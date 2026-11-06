# Named persistent user profiles

Named **user profiles** keep website identity in a persistent Chromium data directory. They are
completely different from the memory profiles selected by `--profile off|balanced|max`: a user
profile chooses *who/where the browser stores its state*, while a memory profile chooses Chromium's
low-RAM flags.

The normal Chromium `Default` profile below the selected data directory persists cookies,
localStorage, IndexedDB, Cache Storage, service workers, and the other state Chromium itself owns.
slimium does not copy that state into JSON and does not require SQLite.

## Registry

The registry is `~/.local/share/slimium/profiles`. Set `SLIM_PROFILES_ROOT=/absolute/path` to
override it (tests always use a fresh temporary override). Its layout is:

```
<root>/.lock                 flock-serializes registry updates
<root>/.current              default selected by 'profile use' (optional)
<root>/NAME/profile.json     mode 0600; preferences and optional tab snapshot
<root>/NAME/data/            mode 0700; Chromium --user-data-dir
<root>/NAME/data/Default/Preferences 0600; Chromium settings, session restore enabled
```

The root and profile directories are mode 0700. Metadata and default-selection updates use a locked,
fsynced temporary file plus atomic rename. Names are 1-64 ASCII letters, digits, `_`, `-`, or
`.`; a leading dot, empty names, slashes, whitespace, and traversal are rejected.

`profile.json` contains only the name, creation/update times, optional user agent and locale, and
(after `profile save`) page URLs, the current-page index, and scroll coordinates. It never receives
cookie values, localStorage values, IndexedDB values, passwords, or tokens read from browser storage.
URLs themselves can be sensitive (including query parameters), so protect metadata as carefully as
browser history. There is deliberately no storage-state export/import command.

## CLI

```sh
slimium profile create NAME [--ua 'User Agent'] [--locale en-US]
slimium profile list
slimium profile use NAME          # default for future 'slimium start'
slimium profile current
slimium profile show NAME
slimium profile rename OLD NEW    # inactive profiles only
slimium profile delete NAME --yes # inactive profiles only; irreversible

slimium start --user NAME [--profile balanced] [--port N]
slimium --session S profile save
slimium --session S profile restore
slimium --session S stop
```

`--session S` is a global option and may also precede `profile`, for example
`slimium --session S profile save`.

Selection order for `slimium start` is:

1. explicit `--user NAME`;
2. `SLIM_USER_PROFILE=NAME` in the environment;
3. the `profile use` default, but only when no explicit `--user-data-dir` was supplied.

A selected named profile always uses `<root>/NAME/data`; it wins over a conflicting
`--user-data-dir`. This makes identity explicit and prevents a launcher default from silently
putting login state in another directory. An explicit `--user-data-dir` without `--user` or
`SLIM_USER_PROFILE` continues to select that directory instead of the `profile use` default.
With no named selection and no `profile use` default, start behavior and arguments are unchanged.

`--ua` and `--locale` are applied as Chromium's `--user-agent` and `--lang` on every named
start. Later explicit Chromium arguments can still override them.

`status` reports both `user` and `user_data_dir` for a named session. The session state file
also records `user`, so subsequent CLI commands do not need to repeat it. Renaming or deleting an
active profile is rejected. Renaming an inactive profile moves its data and updates the default;
deletion requires `--yes`, stays below the validated registry root, and never follows symlinks.

## Session cookies and Chromium session restore

Named profiles also enable Chromium's `session.restore_on_startup=1` in
`data/Default/Preferences`. slimium preserves every other top-level and nested preference and
atomically replaces the file at mode 0600. If the setting is already enabled, the file is only read.
This is required for **session cookies** (cookies without `Max-Age` or `expires`): on a graceful
`Browser.close`, Chromium writes them into its own session state and restores them into the default
browser context on the next start. Persistent cookies, localStorage, and IndexedDB continue to use
Chromium's normal durable stores.

There is no slimium cookie checkpoint, no cookie value in `profile.json`, logs, or command output,
and no background helper. The mechanism works in `slimium start` and `slimium-chrome`, including
with `SLIM_GOV=0`. Off-the-record/isolated browser contexts are not populated from the default
context's restored session.

Chromium's preference also restores tabs, which is independent of cookie persistence:

- ordinary `slimium start --user NAME` closes those automatically restored tabs and keeps the
  established single `about:blank`, so existing startup behavior does not change;
- `slimium start --user NAME -- --restore-last-session` explicitly requests Chromium's tab restore
  and disables that pruning;
- `slimium-chrome` does not prune tabs, so a cu-slim daemon restart can restore pages as well as
  login state. Use cu's `tabs` command to inspect them.

## Save and restore tabs

Persistent login state does **not** require `profile save`: it already lives in Chromium's data
directory and session state. `save` is a separate, explicit tab snapshot:

- record each open page's URL, the session's current page, and scroll X/Y;
- do not record cookies, form contents, storage values, or target IDs (IDs are not stable across
  browser starts);
- on `restore`, open the saved URLs, restore scrolling, activate the saved current page, and close
  the initial `about:blank` page made by `start`.

Run `profile restore` after starting the same named session. Restoring into a browser with other
non-blank pages leaves those pages alone and adds the snapshot; it is not an atomic replacement of
an arbitrary running session. A crash can lose an unsaved tab snapshot and in-flight page state.

## cu-slim / `slimium-chrome`

Create the profile once, then set the name in the **daemon's** environment so its browser inherits
it. cu 1.9.0 has no `cu-slim stop`; do not use legacy `cu-slim start` for this lifecycle. A
separately managed, verified shell lifecycle is:

```sh
slimium profile create instagram
export CU_SLIM_HOME="$HOME/.local/share/cu-slim"
export CU_SLIM_PORT=8790
export CU_SLIM_CDP_PORT=9390
export CU_SLIM_BROWSER="$PWD/slimium-chrome"
export SLIM_USER_PROFILE=instagram
mkdir -p "$CU_SLIM_HOME"

setsid cu-slim daemon >"$CU_SLIM_HOME/daemon.log" 2>&1 </dev/null &
CU_SLIM_PID=$!
# Wait only for the 8790 and 9390 TCP listeners, then verify:
cu-slim diagnostics
cu-slim navigate https://example.com
# ... agent work; session and persistent login state stay in the named profile ...

# Graceful browser close first, then stop only the daemon PID recorded above.
slimium --port "$CU_SLIM_CDP_PORT" cdp Browser.close || true
kill -TERM "$CU_SLIM_PID"
wait "$CU_SLIM_PID"

# Restart a new recorded daemon with the same environment.
setsid cu-slim daemon >"$CU_SLIM_HOME/daemon.log" 2>&1 </dev/null &
CU_SLIM_PID=$!
# Again wait for both TCP listeners before issuing CLI commands.
```

Do not use `cu-slim status` in a readiness loop: a CLI invocation before the daemon exists can
auto-start an unrecorded daemon. The test suite waits for TCP listeners and calls `diagnostics`
only afterwards. On shell exit or SIGINT/SIGTERM, tests close only their recorded browser and daemon.

When `slimium-chrome` has `SLIM_USER_PROFILE` set, it replaces **every** caller-supplied
`--user-data-dir` (including cu's temporary/default directory) with the canonical named data dir.
The identity setting deliberately wins. Without that environment variable, wrapper argument merging
is unchanged. The wrapper still honors `SLIM_PROFILE` independently for memory flags.

The `profile save/restore` CLI operates on slimium-managed sessions containing a `user` field;
a cu-owned browser attached only by `--port` does not acquire that field. cu-slim login persistence
does not need save/restore. Use slimium-managed sessions when explicit URL/current-tab snapshots are
required.

## Lifecycle, isolation, and limitations

- **Graceful close:** `slimium stop` first sends `Browser.close` and waits for Chromium to flush
  cookies and storage. TERM/KILL remain the existing bounded fallbacks if the engine does not exit.
  For cu-slim, use the explicit `Browser.close` + recorded-daemon SIGTERM sequence above; there is
  no `cu-slim stop`.
- **Crash semantics:** durable persistent-cookie/localStorage/IndexedDB transactions generally
  survive SIGKILL, but a crash can lose recently buffered writes, **session cookies**, open tabs,
  scroll positions, and an unsaved `profile save` snapshot. Session-cookie persistence is tested
  and supported across graceful closes, not promised after an arbitrary crash. Use persistent
  (`Max-Age`/expires) cookies when the site permits and close gracefully when durability matters.
- **Tab restore:** the Chromium preference may restore tabs after restart. Only named slimium starts
  prune them by default, and an explicit `-- --restore-last-session` always preserves them.
  `profile restore` does not reconstruct browser history, form contents, or target IDs.
- **One browser per named profile:** slimium checks live process command lines and refuses a second
  browser already using the same named data dir. Chromium's own profile singleton lock remains the
  race-safe backstop if two starts pass the check simultaneously. Different named profiles have
  separate cookie/localStorage/IndexedDB namespaces and can run simultaneously.
- **Encryption:** directory modes are access control, not encryption. Chromium may encrypt some
  cookies using the host OS/keyring, depending on platform and build; slimium adds no encryption and
  does not manage keys. Protect the whole data dir and registry backups. A headless host without an
  OS keyring may have different at-rest protection.
- **No secret export:** registry commands do not print browser storage. `profile show` does print
  saved URLs and preferences. Chromium cookie/session database values are never read by slimium.
- **Real sites:** tests use only a local fixture, never Instagram/Google or a logged-in personal
  browser. The same mechanism applies to real login state because those sites also persist identity
  in Chromium cookies and origin storage below the data dir. Site-specific encryption, expiring
  sessions, service workers, anti-automation controls, and server-side revocation remain outside
  slimium's control.

## Verification and measurements

Run the isolated integration suite with:

```sh
make -j2
tests/profiles.sh
```

It uses fresh temporary roots, CDP 9450-9453, and a loopback fixture server on 9469. It checks
registry validation/permissions/no-secret output, preference preservation and modes, graceful
session-cookie + persistent-cookie + localStorage + IndexedDB persistence, SIGKILL recovery of only
its recorded browser process group, profile isolation and duplicate rejection, explicit and saved
tab restoration, two `SLIM_USER_PROFILE` wrapper restarts with conflicting cu-style data dirs, and
no-follow deletion.

When the optional deployed `~/.local/bin/cu-slim` integration is available, the suite also starts
its own daemon on HTTP 8792 and CDP 9392 with a fresh temporary `CU_SLIM_HOME` and fixture HTTP
9467. It performs a real `Browser.close` + daemon restart and parses cu's nested JSON response.
Missing cu-slim prints an explicit skip; `SLIM_PROFILE_SKIP_CU=1 tests/profiles.sh` skips that
integration deliberately. Occupied required ports fail before any process is launched.

One development run (Chromium 156, `--no-gov`, single samples rather than benchmark medians)
reported: start 225 ms, graceful stop 52 ms, named reopen 232 ms, two-tab restore 64 ms, registry
`profile list` maximum RSS 1880 KB, and stripped binary 151104 bytes. The baseline at commit
`62b1866` was 134648 bytes, an increase of 16456 bytes (12.2%). Across five common `version`
invocations, median maximum RSS was 1744 KB baseline versus 1816 KB current (+72 KB); median
`profile list` RSS was 1696 KB.

The session-cookie follow-up added no helper process and no measured stripped-size increase over
integrated main `7c6e612` (188016 bytes before and after). Five `version` runs had median maximum
RSS 1788 KB baseline versus 1792 KB current (+4 KB). Twenty-five `profile show` runs had median
latency 0.497 ms baseline versus 0.490 ms current, i.e. no measurable preference-read regression.
The script prints fresh lifecycle latency/RSS/size measurements, including real cu stop/restart
timings, on every run. A final integrated run measured 281 ms named start, 53 ms graceful stop,
339 ms named reopen, 51 ms saved-tab restore, 105 ms real cu browser-close/daemon stop, and 314 ms
cu restart-to-diagnostics. These are whole lifecycle times, not incremental preference overhead.
