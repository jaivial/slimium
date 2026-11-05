/* Memory governor for one browser process tree (see docs/GOVERNOR.md).
 * While the tree is idle (its CPU stays below idle_cpu % of one core for idle_ms):
 *  - once per idle period: Memory.simulatePressureNotification critical on the browser and every
 *    page + HeapProfiler.collectGarbage on every page (never Memory.forciblyPurgeJavaScriptMemory);
 *  - every reclaim_s: proactive reclaim of the file pages charged to the tree's own cgroups
 *    ("<N> swappiness=0" -> cgroup v2 memory.reclaim), only for cgroups whose processes (and
 *    those of every descendant cgroup) all belong to the tree;
 *  - opt-in hibernation of tabs beyond the `hibernate` most recent ones (about:blank, restored
 *    with Page.navigateToHistoryEntry when a client attaches to the tab again). */
#ifndef SLIM_GOV_H
#define SLIM_GOV_H
#include <sys/types.h>

typedef struct {
    pid_t root;                    /* browser pid */
    unsigned long long root_start; /* its /proc starttime (0 = read at start) */
    int port;                      /* CDP port; 0 = read <udd>/DevToolsActivePort; -1 = no CDP */
    const char *udd;               /* --user-data-dir of the browser (for port 0) */
    int idle_ms;                   /* SLIM_GOV_IDLE_MS (5000) */
    double idle_cpu;               /* SLIM_GOV_IDLE_CPU, % of one core (5) */
    int reclaim_s;                 /* SLIM_GOV_RECLAIM_S (30): reclaim at least this often while idle; 0 = never */
    int grow_mb;                   /* SLIM_GOV_RECLAIM_GROW_MB (24): reclaim early once the reclaimed cgroups'
                                      mapped file pages grew this much since the last reclaim; 0 = timer only */
    int min_s;                     /* SLIM_GOV_RECLAIM_MIN_S (5): never reclaim more often than this */
    int gc;                        /* SLIM_GOV_GC (1): pressure + GC pass */
    int hibernate;                 /* SLIM_GOV_HIBERNATE (0 = off): keep K most recent tabs */
    int hib_min_s;                 /* SLIM_GOV_HIBERNATE_MIN_S (120): untouched at least this long */
    int busy_ms;                   /* SLIM_GOV_BUSY_MS (1000): tree CPU time above idle_cpu that counts as
                                      real activity (re-arms the GC pass and an early reclaim) */
    int wait_engine;               /* wrapper mode: root is still slimium/systemd-run until it execs */
    int measure;                   /* SLIM_GOV_MEASURE (0; 1 in the foreground CLI): iso before/after each
                                      pass in the events (two pagemap scans of the tree per pass) */
    int out_fd;                    /* events (text, or JSON with json=1); -1 = none */
    int json;
    int log_fd;                    /* SLIM_GOV_LOG (JSON lines), -1 = none */
} slim_gov_cfg;

typedef struct {
    long iso_before_kb, iso_after_kb, file_before_kb, file_after_kb, anon_before_kb, anon_after_kb;
    long reclaimed_kb;    /* sum over reclaimed cgroups of (file pages before - after) */
    int cgroups, cgroups_ok, foreign; /* cgroups of the tree / reclaimed / skipped (foreign processes) */
    int tabs_gc, pressure, hibernated, procs;
    long ms;              /* duration of the pass */
} slim_gov_pass;

/* Defaults from the SLIM_GOV_* environment. */
void slim_gov_defaults(slim_gov_cfg *g);
/* Govern until the root process exits (or SIGINT/SIGTERM). Returns 0. */
int slim_gov_run(slim_gov_cfg *g);
/* One pass now (pressure + GC, hibernation if enabled, reclaim if reclaim_s > 0). 0 ok. */
int slim_gov_once(slim_gov_cfg *g, slim_gov_pass *r);

/* ---- launch helpers (wrapper and 'slimium start') ---- */
/* CDP port named in a Chromium argv: N, 0 (DevToolsActivePort), or -1 (none/pipe). *udd = the
 * --user-data-dir value (view into argv) or NULL. */
int slim_gov_argv_port(int argc, char **argv, const char **udd);
/* Fork a detached governor (double fork: not a child of the browser, own session, stdio on
 * /dev/null, fresh exec of slimium as 'slimium gov --pid ROOT ...') for root, which may still be
 * about to exec the engine (wait_engine). Returns the governor pid (> 0), 0 if SLIM_GOV=0, -1 on
 * error. The caller's own process is not otherwise affected (no fd, no child left behind). */
pid_t slim_gov_spawn(pid_t root, int port, const char *udd, int wait_engine);
/* argv that runs av (av[0] = absolute engine path) in its own systemd user scope without changing
 * the pid ("systemd-run --user --scope ... -- av"), after a probe that systemd-run works here.
 * NULL if SLIM_SCOPE=0, systemd-run is missing or the probe fails (then exec av unscoped).
 * Returns one malloc'd pointer array (free once; strings point into av / static storage,
 * except the unit name which lives in the block). */
char **slim_scope_argv(char **av, pid_t pid);
/* posix_fadvise(DONTNEED) the engine ELF and the resources next to it (.so .pak .dat .bin,
 * locales/): drops only unmapped cached pages, so this instance faults (and is charged for) its
 * own copy. SLIM_GOV_FADVISE=0 disables. Returns the number of files advised. */
int slim_engine_fadvise(const char *engine);
/* Private copy of the engine ELF (reflink/copy_file_range; the other files of its directory are
 * symlinks) in $XDG_CACHE_HOME/slimium/engines/<name>-<dev>-<ino>-<size>-<mtime>[.N]/: its page
 * cache belongs to this copy's inode only, so it is charged to (and reclaimable from) the
 * browser's own cgroups even while other Chromiums map the system ELF. One slot per concurrently
 * running browser (SLIM_ENGINE_COPIES, default 4; a slot is busy while some process has it as
 * its exe). Reused while the source is unchanged; copies of other versions are removed.
 * SLIM_ENGINE_COPY=0 disables. malloc'd path or NULL (use engine). */
char *slim_engine_private(const char *engine);
/* pid of a running governor for root, or 0. */
pid_t slim_gov_find(pid_t root);

#endif
