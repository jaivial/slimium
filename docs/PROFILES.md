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
<root>/NAME/profile.json     mode 0600; preferences, pinned password store, optional tab snapshot
<root>/NAME/data/            mode 0700; Chromium --user-data-dir
<root>/NAME/data/Default/Preferences 0600; Chromium settings, session restore enabled
```

The root and profile directories are mode 0700. Metadata and default-selection updates use a locked,
fsynced temporary file plus atomic rename. Names are 1-64 ASCII letters, digits, `_`, `-`, or
`.`; a leading dot, empty names, slashes, whitespace, and traversal are rejected.

`profile.json` contains only the name, creation/update times, optional user agent and locale, the
pinned `password_store` (see below), and (after `profile save`) page URLs, the current-page index, and scroll coordinates. It never receives
cookie values, localStorage values, IndexedDB values, passwords, or tokens read from browser storage.
URLs themselves can be sensitive (including query parameters), so protect metadata as carefully as
browser history. There is deliberately no storage-state export/import command.

## CLI

```sh
slimium profile create NAME [--ua 'User Agent'] [--locale en-US] [--password-store basic|gnome-libsecret|kwallet5|kwallet6]
slimium profile list
slimium profile use NAME          # default for future 'slimium start'
slimium profile current
slimium profile show NAME
slimium profile rename OLD NEW    # inactive profiles only
slimium profile delete NAME --yes # inactive profiles only; irreversible
slimium profile password-store NAME STORE  # re-pin an inactive profile (refused if it would lose cookies)

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

## Password store (cookie encryption) pinning

Chromium on Linux encrypts every cookie value (and saved passwords) with a key chosen by
`--password-store`:

| store | key | value prefix |
|---|---|---|
| `basic` | a fixed key compiled into Chromium | `v10` |
| `gnome-libsecret`, `kwallet5`, `kwallet6` | a random key kept in the desktop keyring | `v11` |

Without the switch Chromium picks the keyring of the current desktop (GNOME with a session D-Bus
→ libsecret, `v11`) or `basic` when there is none. A Chromium launched with a store that cannot
decrypt an existing cookie **deletes that cookie**. A keyring store can still read `v10` values,
but `basic` can never read `v11` values, and one keyring cannot read another keyring's `v11`.

This caused real data loss: `slimium start --user NAME` used to launch Chromium without
`--password-store` (so on GNOME the login cookies were written as `v11`), while cu launches its
browser through `slimium-chrome` with `--password-store=basic`. Opening the same profile under cu
deleted the keyring-encrypted login cookies; only the cookies cu wrote afterwards (`v10`) remained.

Every named profile therefore pins its store in `profile.json` (`"password_store"`):

- **New profiles default to `basic`.** It is deterministic and works headless, under systemd, over
  SSH, and under cu (which forces `basic`). `basic` is *obfuscation, not encryption*: the key is
  public. The protection of a `basic` profile is the 0700 registry/data directories (and disk
  encryption); treat the data dir like a password file. `profile create NAME --password-store
  gnome-libsecret` (or `kwallet5`/`kwallet6`) chooses a keyring explicitly; such a profile then
  needs the unlocked keyring and a session D-Bus on every launch.
- **Legacy profiles** (created before the field existed) get it detected on first use (any launch
  or `profile show`): if any cookie in `data/Default/Cookies` carries the `v11` prefix, the
  desktop's keyring store (`kwallet6`/`kwallet5` on KDE per `KDE_SESSION_VERSION`, otherwise
  `gnome-libsecret`), else `basic`. The value is persisted atomically (locked registry, fsync,
  rename) and a one-line note is printed on stderr. A database that cannot be parsed is not
  guessed: the command fails and asks for an explicit `profile password-store NAME STORE`.
- **Enforcement:** `slimium start --user NAME` (and the `profile use` default) always passes
  `--password-store=<pinned>`. `slimium-chrome` with `SLIM_USER_PROFILE=NAME` **replaces** every
  caller `--password-store` (`--password-store=X`, `-password-store=X`, `--password-store X`) with
  the pinned value, exactly as it replaces `--user-data-dir`: the identity setting wins over the
  launcher. A caller value that differed is logged on stderr, e.g.
  `slimium-chrome: user profile 'NAME' pins --password-store=basic; replaced caller
  --password-store=gnome-libsecret`. Extra arguments to `start --user` are treated the same way.
  Without `--user`, `SLIM_USER_PROFILE`, or a `profile use` default nothing changes: caller
  switches pass through untouched and no switch is added.
- **Guard:** before every named launch slimium counts the encryption prefixes in the cookie
  database (read-only). If the pin is `basic` but `v11` cookies exist (for example a hand-edited
  `profile.json`), or the pin is a keyring and `v11` cookies exist but no session D-Bus is
  reachable, the launch is refused with an explicit error *before Chromium starts*, so nothing
  is deleted. `profile password-store NAME STORE` re-pins an inactive profile and refuses any
  change that would make existing `v11` cookies unreadable. If the database cannot be parsed, a
  pinned profile still launches (warning on stderr): the pin is the primary protection.
- `profile show` prints `password_store` and the number of `v10`, `v11`, and other encrypted
  cookies (counts only; no cookie name or value is read out). `profile list --json` includes the
  pinned value; `start --json` reports `password_store` for named sessions.

The cookie database is parsed by a small dependency-free reader of the SQLite file format
(`src/cookiedb.c`: b-tree pages, records, overflow chains, and WAL frames left by a crash). It
opens the file `O_RDONLY`/`O_NOFOLLOW`, compares only the first three bytes of each
`encrypted_value`, never decrypts anything, and needs neither libsqlite3 nor the `sqlite3` CLI.
Chromium 156 keeps the database in rollback-journal mode; a hot journal left by a crash is not
replayed (the scan sees the main file).

### cu and keyring profiles

cu forces `--password-store=basic`. With the default `basic` pin this agrees with cu. A profile
pinned to a keyring keeps using that keyring under cu too (the wrapper replaces cu's `basic`), so it
works only where cu's daemon has the user's session D-Bus and an unlocked keyring; a systemd user
service started outside the graphical session usually lacks `DBUS_SESSION_BUS_ADDRESS`, and the
guard then refuses to launch rather than lose the cookies. Prefer `basic` for agent profiles.

### Recovery

Cookies that Chromium has already deleted **cannot be recovered**: the deletion is in Chromium's
own database and slimium keeps no copy (by design it never exports cookie values). Log in again
(for example `slimium start --user NAME --headful`). With the fix the profile is pinned, so the new
login stays readable by both `slimium start` and cu. To move a keyring-pinned profile to `basic`
there is no conversion: create a new `basic` profile and log in there.

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
- **Encryption:** directory modes are access control, not encryption. Chromium encrypts cookies
  with the pinned password store (above); with `basic` that is obfuscation with a public key.
  slimium adds no encryption and does not manage keys. Protect the whole data dir and registry
  backups.
- **No secret export:** registry commands do not print browser storage. `profile show` does print
  saved URLs and preferences. slimium reads only the 3-byte encryption prefix of each cookie in
  Chromium's cookie database (to pin and guard the password store); values are never read out,
  decrypted, logged, or printed.
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

Password-store checks in the same suite: new profiles pin `basic`; a named start overriding a caller
`--password-store`; persistent + session cookies + localStorage written by `start` and read back
through the wrapper with a caller `--password-store=basic`, `--password-store=gnome-libsecret`, and
none (database stays `v10` only, our scanner matches SQLite's own count); argv rewriting of all
switch spellings via a fake engine, and the unnamed wrapper unchanged; the mismatch guard (start and
wrapper refuse, database byte-identical, no engine executed, no-D-Bus refusal, re-pin rules);
legacy detection from crafted `v10`/`v11`/KDE/no-database/corrupt fixtures (made with python3's
stdlib `sqlite3`, test-only) and from a real Chromium profile whose pin was stripped. When a usable
keyring exists, a `gnome-libsecret` profile is written, then opened cu-style with
`--password-store=basic` (pinned and legacy/stripped) and its `v11` cookies survive; otherwise the
suite prints `== keyring checks skipped: ...`. `make unit` checks the scanner against SQLite files
generated by the `sqlite3` CLI (page sizes 512/4096/65536, overflow chains, an uncheckpointed WAL)
and 400 corrupted/truncated variants (skipped with a message when the CLI is absent).

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
