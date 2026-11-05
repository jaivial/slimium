/* 'slimium gov': run the memory governor (src/gov.c) for a browser, in the foreground or once.
 *   slimium gov [--session S | --port N --pid P] [--once] [--reclaim-s N] [--idle-ms N]
 *               [--idle-cpu PCT] [--hibernate K] [--no-gc] [--json]
 * Internal (spawned by slimium-chrome / slimium start): --daemon --pid P --pid-start T
 *   --cdp N|none [--udd DIR] [--wait-engine]  (nothing on stdout; events to SLIM_GOV_LOG). */
#include "cmd.h"
#include "gov.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_gov(ctx_t *c, int argc, char **argv);

static int num_opt(ctx_t *c, const char *name, const char *v, long lo, long hi, int *out) {
    long n;
    if (!v) return 0;
    if (parse_int(v, &n) || n < lo || n > hi) return usage_err(c, "bad value for %s: %s", name, v);
    *out = (int)n;
    return 0;
}

int cmd_gov(ctx_t *c, int argc, char **argv) {
    const char *pid_s = opt_val(c, &argc, argv, "--pid");
    const char *start_s = opt_val(c, &argc, argv, "--pid-start");
    const char *cdp_s = opt_val(c, &argc, argv, "--cdp");
    const char *udd = opt_val(c, &argc, argv, "--udd");
    const char *rs = opt_val(c, &argc, argv, "--reclaim-s");
    const char *im = opt_val(c, &argc, argv, "--idle-ms");
    const char *ic = opt_val(c, &argc, argv, "--idle-cpu");
    const char *hb = opt_val(c, &argc, argv, "--hibernate");
    const char *gm = opt_val(c, &argc, argv, "--grow-mb");
    int once = opt_flag(&argc, argv, "--once");
    int daemon = opt_flag(&argc, argv, "--daemon");
    int wait_engine = opt_flag(&argc, argv, "--wait-engine");
    int no_gc = opt_flag(&argc, argv, "--no-gc");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "gov takes no positional arguments");
    slim_gov_cfg g;
    slim_gov_defaults(&g);
    int root = 0;
    if (num_opt(c, "--reclaim-s", rs, 0, 86400, &g.reclaim_s) || num_opt(c, "--idle-ms", im, 0, 86400000, &g.idle_ms) ||
        num_opt(c, "--hibernate", hb, 0, 1000, &g.hibernate) || num_opt(c, "--grow-mb", gm, 0, 1 << 20, &g.grow_mb) || num_opt(c, "--pid", pid_s, 1, 1 << 30, &root))
        return -1;
    if (ic) {
        double d;
        if (parse_double(ic, &d) || d <= 0 || d > 10000) return usage_err(c, "bad value for --idle-cpu: %s", ic);
        g.idle_cpu = d;
    }
    if (no_gc) g.gc = 0;
    if (start_s) g.root_start = strtoull(start_s, NULL, 10);
    g.udd = udd;
    if (cdp_s) {
        long n;
        if (!strcmp(cdp_s, "none")) g.port = -1;
        else if (!parse_int(cdp_s, &n) && n >= 0 && n < 65536) g.port = (int)n;
        else return usage_err(c, "bad value for --cdp: %s (N|none)", cdp_s);
    } else if (c->port) g.port = c->port;
    else if (!root) {
        if (ctx_state(c)) return -1;
        g.port = c->st.port;
        root = c->st.attached || !c->st.pid ? find_browser_pid(c->st.port) : c->st.pid;
        if (!g.udd) g.udd = c->st.udd;
    }
    if (!root && g.port > 0) root = find_browser_pid(g.port);
    if (root <= 0) return slim_seterr("no browser found (use --session S, --port N or --pid P)");
    if (kill(root, 0) && !daemon) return slim_seterr("browser pid %d is not running", root);
    g.root = root;
    g.wait_engine = wait_engine;
    if (daemon) g.out_fd = -1;
    else { g.out_fd = once ? -1 : 1; g.json = c->json; g.measure = 1; }
    if (once) {
        slim_gov_pass r;
        if (slim_gov_once(&g, &r)) return -1;
        out_i(c, "pid", root); out_i(c, "port", g.port);
        out_i(c, "iso_before_kb", r.iso_before_kb); out_i(c, "iso_after_kb", r.iso_after_kb);
        out_i(c, "anon_before_kb", r.anon_before_kb); out_i(c, "anon_after_kb", r.anon_after_kb);
        out_i(c, "file_union_before_kb", r.file_before_kb); out_i(c, "file_union_after_kb", r.file_after_kb);
        out_i(c, "reclaimed_kb", r.reclaimed_kb); out_i(c, "cgroups", r.cgroups);
        out_i(c, "cgroups_reclaimed", r.cgroups_ok); out_i(c, "cgroups_foreign", r.foreign);
        out_i(c, "tabs_gc", r.tabs_gc); out_b(c, "pressure", r.pressure); out_i(c, "hibernated", r.hibernated);
        out_i(c, "procs", r.procs); out_i(c, "ms", r.ms);
        out_t(c, "iso %.1f -> %.1f MB (anon+shmem %.1f -> %.1f, file_union %.1f -> %.1f); reclaimed %.1f MB in %d/%d cgroups (%d foreign skipped); gc %d tabs, pressure %s%s; %ld ms",
              r.iso_before_kb / 1024.0, r.iso_after_kb / 1024.0, r.anon_before_kb / 1024.0, r.anon_after_kb / 1024.0,
              r.file_before_kb / 1024.0, r.file_after_kb / 1024.0, r.reclaimed_kb / 1024.0, r.cgroups_ok, r.cgroups, r.foreign,
              r.tabs_gc, r.pressure ? "sent" : "no", r.hibernated ? ", tabs hibernated" : "", r.ms);
        return 0;
    }
    slim_gov_run(&g);
    if (daemon) return 0;
    out_i(c, "pid", root);
    out_t(c, "governor stopped (browser pid %d)", root);
    return 0;
}
