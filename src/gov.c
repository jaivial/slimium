/* Memory governor: idle detection by tree CPU, pressure + GC over CDP, cgroup v2 proactive
 * reclaim of file pages limited to cgroups that hold nothing but the governed tree, opt-in tab
 * hibernation, and the launch helpers (detached spawn, systemd scope, fadvise).
 * Linux only, libc + POSIX. See docs/GOVERNOR.md. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gov.h"
#include "cdp.h"
#include "json.h"
#include "mem.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXP 256          /* processes of one tree */
#define MAXCG 16          /* cgroups of one tree */
#define MAXTABS 128
#define CHUNK_KB (64L * 1024)  /* memory.reclaim request size; activity is checked between chunks */
#define SCAN_MS 5000      /* full /proc rescan of the tree */
#define GRACE_MS 3000     /* CPU samples right after our own pass are not "activity" */
#define CG_ROOT "/sys/fs/cgroup"

typedef struct { pid_t pid; unsigned long long start, ticks; } pinfo;
/* ours: we attached to it and have not seen our detach yet (its attach events are ours) */
typedef struct { char id[48]; long long last; int hib, attached, seen, want, ours; long long entry; } tabrec;
typedef struct { pid_t ppid; unsigned long long ticks, start; char state; char comm[16]; } pst;

typedef struct {
    slim_gov_cfg *cfg;
    pinfo set[MAXP];
    int n;
    pid_t crash, crash_mon;   /* Chromium's crashpad handler + its self-monitor (reparented away) */
    int crash_tries;
    long hz;
    long long last_scan;
    cdp_t cdp;
    int cdp_open;
    tabrec tabs[MAXTABS];
    int ntabs;
    long long quiet_until;    /* target events before this are our own doing */
    int reclaim_off;          /* kernel lacks memory.reclaim swappiness= */
    char okcg[MAXCG][PATH_MAX / 4];  /* cgroup dirs reclaimed last time (adaptive trigger) */
    int nok;
    long post_kb;             /* their file pages right after that reclaim */
} gov_t;

static volatile sig_atomic_t g_stop;
static void on_stop(int s) { (void)s; g_stop = 1; }

/* ---- /proc helpers ---- */
static long rdb(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (long)n;
}

static int pstat(pid_t pid, pst *s) {
    char p[48], b[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    if (rdb(p, b, sizeof b) <= 0) return -1;
    char *l = strchr(b, '('), *r = strrchr(b, ')');
    if (!l || !r || r < l || r[1] != ' ') return -1;
    size_t cl = (size_t)(r - l - 1);
    if (cl > 15) cl = 15;
    memcpy(s->comm, l + 1, cl); s->comm[cl] = 0;
    char *e = r + 2;
    s->state = *e;
    while (*e && *e != ' ') e++;
    unsigned long long v[20] = {0};
    for (int k = 1; k < 20 && *e; k++) v[k] = strtoull(e, &e, 10);  /* field k+3 of stat(5) */
    s->ppid = (pid_t)v[1]; s->ticks = v[11] + v[12]; s->start = v[19];
    return 0;
}

static int alive_as(pid_t pid, unsigned long long start) {
    pst s;
    return pstat(pid, &s) == 0 && s.start == start && s.state != 'Z' && s.state != 'X';
}

static int set_has(const gov_t *g, pid_t p) {
    for (int i = 0; i < g->n; i++) if (g->set[i].pid == p) return 1;
    return 0;
}

/* Rebuild the governed set: root + descendants (from one /proc scan) + the crashpad handler named
 * by a tree member (--crashpad-handler-pid=N) and its self-monitor (same parent, same cgroup,
 * started within a second). Tick baselines of processes already known are kept. */
typedef struct { pid_t pid, ppid; unsigned long long start, ticks; char comm[16]; } prow;
static int cg_of(pid_t pid, char *out, size_t cap);

static void scan_tree(gov_t *g) {
    prow *rows = NULL; size_t nr = 0, cap = 0;
    DIR *d = opendir("/proc");
    g->last_scan = now_ms();
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pst s;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pstat(pid, &s) || s.state == 'Z') continue;
        if (nr == cap) {
            size_t nc = cap ? cap * 2 : 1024;
            prow *t = realloc(rows, nc * sizeof *t);
            if (!t) break;
            rows = t; cap = nc;
        }
        rows[nr].pid = pid; rows[nr].ppid = s.ppid; rows[nr].start = s.start; rows[nr].ticks = s.ticks;
        memcpy(rows[nr].comm, s.comm, sizeof s.comm);
        nr++;
    }
    closedir(d);
    pinfo old[MAXP]; int on = g->n;
    memcpy(old, g->set, sizeof(pinfo) * (size_t)on);
    g->n = 0;
    char *in = calloc(nr ? nr : 1, 1);
    if (!in) { free(rows); return; }
    for (size_t i = 0; i < nr; i++)
        if (rows[i].pid == g->cfg->root && rows[i].start == g->cfg->root_start) in[i] = 1;
    for (int grew = 1; grew;) {          /* descendants: passes until nothing new */
        grew = 0;
        for (size_t i = 0; i < nr; i++) {
            if (in[i]) continue;
            for (size_t j = 0; j < nr; j++)
                if (in[j] && rows[j].pid == rows[i].ppid) { in[i] = 1; grew = 1; break; }
        }
    }
    /* crashpad: the pid is on the command line of the zygotes/renderers */
    if (!g->crash && g->crash_tries < 6) {
        g->crash_tries++;
        for (size_t i = 0; i < nr && !g->crash; i++) {
            if (!in[i] || rows[i].pid == g->cfg->root) continue;
            char p[48], *cl; size_t n;
            snprintf(p, sizeof p, "/proc/%d/cmdline", (int)rows[i].pid);
            if (read_file(p, &cl, &n, 1 << 16)) continue;
            for (size_t k = 0; k < n; k++) if (!cl[k]) cl[k] = ' ';
            char *c = strstr(cl, "--crashpad-handler-pid=");
            if (c) g->crash = (pid_t)atoi(c + 23);
            free(cl);
        }
    }
    if (g->crash) {
        size_t ci = nr;
        for (size_t i = 0; i < nr; i++) if (rows[i].pid == g->crash) ci = i;
        if (ci < nr && !strncmp(rows[ci].comm, "chrome_crashpad", 15)) {
            in[ci] = 1;
            char cg0[512], cg1[512];
            int hcg = cg_of(rows[ci].pid, cg0, sizeof cg0) == 0;
            for (size_t i = 0; i < nr; i++) {
                if (i == ci || rows[i].ppid != rows[ci].ppid || strncmp(rows[i].comm, "chrome_crashpad", 15)) continue;
                unsigned long long dt = rows[i].start > rows[ci].start ? rows[i].start - rows[ci].start : rows[ci].start - rows[i].start;
                if (dt > (unsigned long long)g->hz || !hcg || cg_of(rows[i].pid, cg1, sizeof cg1) || strcmp(cg0, cg1)) continue;
                in[i] = 1; g->crash_mon = rows[i].pid;
            }
        } else g->crash = 0;
    }
    for (size_t i = 0; i < nr && g->n < MAXP; i++) {
        if (!in[i]) continue;
        pinfo *p = &g->set[g->n++];
        p->pid = rows[i].pid; p->start = rows[i].start; p->ticks = rows[i].ticks;
        for (int k = 0; k < on; k++)
            if (old[k].pid == p->pid && old[k].start == p->start) { p->ticks = old[k].ticks; break; }
    }
    free(in); free(rows);
}

/* CPU of the governed set since the last sample, in % of one core. Gone processes drop out. */
static double sample_cpu(gov_t *g, long long *tprev) {
    unsigned long long d = 0;
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        pst s;
        if (pstat(g->set[i].pid, &s) || s.start != g->set[i].start || s.state == 'Z') continue;
        if (s.ticks > g->set[i].ticks) d += s.ticks - g->set[i].ticks;
        g->set[i].ticks = s.ticks;
        g->set[k++] = g->set[i];
    }
    g->n = k;
    long long t = now_ms(), el = t - *tprev;
    *tprev = t;
    if (el <= 0) el = 1;
    return (double)d * 100.0 * 1000.0 / (double)g->hz / (double)el;
}

/* ---- events (SLIM_GOV_LOG: JSON lines; out_fd: text or JSON) ---- */
typedef struct { sbuf j, t; } ev_t;
static void ev_new(gov_t *g, ev_t *e, const char *name) {
    memset(e, 0, sizeof *e);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; time_t s = ts.tv_sec; localtime_r(&s, &tm);
    sb_printf(&e->j, "{\"ts\":%lld,\"ev\":\"%s\",\"pid\":%d", (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, name, (int)g->cfg->root);
    sb_printf(&e->t, "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, name);
}
static void ev_i(ev_t *e, const char *k, long long v) { sb_printf(&e->j, ",\"%s\":%lld", k, v); sb_printf(&e->t, " %s=%lld", k, v); }
static void ev_kb(ev_t *e, const char *k, long kb) { sb_printf(&e->j, ",\"%s_kb\":%ld", k, kb); sb_printf(&e->t, " %s=%.1fMB", k, (double)kb / 1024.0); }
static void ev_d(ev_t *e, const char *k, double v) { sb_printf(&e->j, ",\"%s\":%.2f", k, v); sb_printf(&e->t, " %s=%.1f", k, v); }
static void ev_s(ev_t *e, const char *k, const char *v) {
    sb_printf(&e->j, ",\"%s\":", k); sb_jstrz(&e->j, v);
    sb_printf(&e->t, " %s=%s", k, v ? v : "-");
}
static void wr(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; p += w; n -= (size_t)w; }
}
static void ev_end(gov_t *g, ev_t *e) {
    sb_puts(&e->j, "}\n"); sb_putc(&e->t, '\n');
    if (g->cfg->log_fd >= 0) wr(g->cfg->log_fd, e->j.p, e->j.n);
    if (g->cfg->out_fd >= 0) { if (g->cfg->json) wr(g->cfg->out_fd, e->j.p, e->j.n); else wr(g->cfg->out_fd, e->t.p, e->t.n); }
    sb_free(&e->j); sb_free(&e->t);
}
static void ev_msg(gov_t *g, const char *name, const char *k, const char *v) {
    ev_t e; ev_new(g, &e, name); ev_s(&e, k, v); ev_end(g, &e);
}

/* ---- cgroups ---- */
static int cg_of(pid_t pid, char *out, size_t cap) {
    char p[48], b[2048];
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    if (rdb(p, b, sizeof b) <= 0) return -1;
    for (char *l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        if (strncmp(l, "0::", 3)) continue;
        l += 3;
        size_t n = strcspn(l, "\n");
        if (!n || n >= cap) return -1;
        memcpy(out, l, n); out[n] = 0;
        return 0;
    }
    return -1;   /* cgroup v1 only */
}

/* Every process of the cgroup dir and of all its descendant cgroups is in the governed set.
 * (memory.reclaim acts on the whole subtree, so a foreign process anywhere below disqualifies.) */
static int cg_only_ours(const gov_t *g, const char *dir, int depth) {
    char p[PATH_MAX], *b; size_t n;
    if (depth > 8 || snprintf(p, sizeof p, "%s/cgroup.procs", dir) >= (int)sizeof p) return 0;
    if (read_file(p, &b, &n, 1 << 20)) return 0;
    int ok = 1;
    for (char *s = b; ok && *s;) {
        char *e; long v = strtol(s, &e, 10);
        if (e == s) { s++; continue; }
        if (v > 0 && !set_has(g, (pid_t)v)) ok = 0;
        s = e;
    }
    free(b);
    if (!ok) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while (ok && (de = readdir(d))) {
        if (de->d_type != DT_DIR || de->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) { ok = 0; break; }
        ok = cg_only_ours(g, p, depth + 1);
    }
    closedir(d);
    return ok;
}

static long stat_kb(const char *dir, const char *key) {
    char p[PATH_MAX], b[8192];
    snprintf(p, sizeof p, "%s/memory.stat", dir);
    if (rdb(p, b, sizeof b) <= 0) return -1;
    size_t kl = strlen(key);
    for (char *l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == ' ') return (long)(strtoull(l + kl + 1, NULL, 10) / 1024);
    return -1;
}
static long cg_file_kb(const char *dir) {
    long a = stat_kb(dir, "active_file"), i = stat_kb(dir, "inactive_file");
    return a < 0 || i < 0 ? -1 : a + i;
}

static void reclaim(gov_t *g, slim_gov_pass *r) {
    scan_tree(g);
    char cgs[MAXCG][512];
    int ncg = 0;
    for (int i = 0; i < g->n; i++) {
        char c[512];
        if (cg_of(g->set[i].pid, c, sizeof c)) continue;
        int dup = 0;
        for (int k = 0; k < ncg; k++) if (!strcmp(cgs[k], c)) dup = 1;
        if (!dup && ncg < MAXCG) memcpy(cgs[ncg++], c, sizeof c);
    }
    r->cgroups = ncg;
    g->nok = 0; g->post_kb = 0;
    double abort_cpu = g->cfg->idle_cpu * 4 > 20 ? g->cfg->idle_cpu * 4 : 20;
    for (int k = 0; k < ncg; k++) {
        char dir[PATH_MAX - 32], rf[PATH_MAX];
        ev_t e;
        if (!strcmp(cgs[k], "/") || snprintf(dir, sizeof dir, CG_ROOT "%s", cgs[k]) >= (int)sizeof dir) continue;
        snprintf(rf, sizeof rf, "%s/memory.reclaim", dir);
        if (!cg_only_ours(g, dir, 0)) { r->foreign++; ev_msg(g, "skip-foreign", "cgroup", cgs[k]); continue; }
        if (access(rf, W_OK)) { ev_msg(g, "skip-no-reclaim", "cgroup", cgs[k]); continue; }
        long before = cg_file_kb(dir), want = before;
        int stop = 0, err = 0, again = 0, chunks = 0;
        long long t0 = now_ms(), tp = t0;
        sample_cpu(g, &tp);   /* baseline for the activity check */
        while (want > 0 && !stop) {
            long ch = want < CHUNK_KB ? want : CHUNK_KB;
            if (!cg_only_ours(g, dir, 0)) { r->foreign++; ev_msg(g, "skip-foreign", "cgroup", cgs[k]); break; } /* re-check before every write */
            char cmd[64];
            int cl = snprintf(cmd, sizeof cmd, "%ldK swappiness=0", ch);
            int fd = open(rf, O_WRONLY | O_CLOEXEC);
            if (fd < 0) { err = errno; break; }
            ssize_t w = write(fd, cmd, (size_t)cl);
            int we = errno;
            close(fd);
            chunks++;
            if (w < 0) {
                if (we == EAGAIN) again = 1;        /* nothing more reclaimable: done */
                else { err = we; if (we == EINVAL) g->reclaim_off = 1; }
                break;
            }
            want -= ch;
            /* the browser woke up (judged over >= 250 ms: ticks are 10 ms): leave it alone */
            if (now_ms() - tp >= 250 && sample_cpu(g, &tp) > abort_cpu) stop = 1;
        }
        long after = cg_file_kb(dir);
        if (before > 0 && after >= 0 && before > after) r->reclaimed_kb += before - after;
        r->cgroups_ok++;
        if (after >= 0 && g->nok < MAXCG && strlen(dir) < sizeof g->okcg[0]) {
            memcpy(g->okcg[g->nok++], dir, strlen(dir) + 1);
            g->post_kb += after;
        }
        ev_new(g, &e, "reclaim");
        ev_s(&e, "cgroup", cgs[k]); ev_kb(&e, "file_before", before); ev_kb(&e, "file_after", after);
        ev_i(&e, "chunks", chunks); ev_i(&e, "ms", now_ms() - t0);
        if (again) ev_s(&e, "end", "eagain");
        if (stop) ev_s(&e, "end", "activity");
        if (err) ev_s(&e, "error", strerror(err));
        ev_end(g, &e);
        if (stop) break;
    }
}

/* Growth of the file pages of the cgroups reclaimed last time (kB); -1 if unknown. */
static long file_growth(gov_t *g) {
    if (!g->nok) return -1;
    long s = 0;
    for (int i = 0; i < g->nok; i++) { long v = cg_file_kb(g->okcg[i]); if (v < 0) return -1; s += v; }
    return s - g->post_kb;
}

/* ---- DevTools ---- */
static int resolve_port(gov_t *g) {
    if (g->cfg->port != 0) return g->cfg->port;
    char p[PATH_MAX], b[64];
    if (g->cfg->udd && *g->cfg->udd) snprintf(p, sizeof p, "%s/DevToolsActivePort", g->cfg->udd);
    else {
        const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
        if (x && *x == '/') snprintf(p, sizeof p, "%s/chromium/DevToolsActivePort", x);
        else if (h && *h) snprintf(p, sizeof p, "%s/.config/chromium/DevToolsActivePort", h);
        else return -1;
    }
    if (rdb(p, b, sizeof b) <= 0) return -1;
    int v = atoi(b);
    return v > 0 && v < 65536 ? v : -1;
}

static void cdp_done(gov_t *g) {
    if (g->cdp_open) cdp_close(&g->cdp);
    g->cdp_open = 0;
}

static int cdp_up(gov_t *g) {
    if (g->cdp_open && g->cdp.open) return 0;
    cdp_done(g);
    int port = resolve_port(g);
    if (port <= 0) return -1;
    char *url = NULL;
    if (cdp_browser_url(port, 2000, &url, NULL)) return -1;
    int r = cdp_connect(&g->cdp, url, 2000);
    free(url);
    if (r) return -1;
    g->cdp_open = 1;
    if (g->cfg->hibernate > 0) {   /* target events drive recency and restore */
        g->quiet_until = now_ms() + 1000;
        if (cdp_call(&g->cdp, NULL, "Target.setDiscoverTargets", "{\"discover\":true}", 5000, NULL, NULL)) { cdp_done(g); return -1; }
    }
    return 0;
}

static tabrec *tab_get(gov_t *g, const char *id, int create) {
    for (int i = 0; i < g->ntabs; i++) if (!strcmp(g->tabs[i].id, id)) return &g->tabs[i];
    if (!create || g->ntabs >= MAXTABS || strlen(id) >= sizeof g->tabs[0].id) return NULL;
    tabrec *t = &g->tabs[g->ntabs++];
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    t->last = now_ms();
    return t;
}

static void tab_event(gov_t *g, jv m, jv pr) {
    long long t = now_ms();
    if (json_str_eq(m, "Target.targetDestroyed")) {
        char *id = json_get_str(pr, "targetId");
        tabrec *r = id ? tab_get(g, id, 0) : NULL;
        if (r) *r = g->tabs[--g->ntabs];
        free(id);
        return;
    }
    jv ti, a;
    if (!json_get(pr, "targetInfo", &ti) || !json_streq_path(ti, "type", "page")) return;
    char *id = json_get_str(ti, "targetId"), *url = json_get_str(ti, "url");
    tabrec *r = id ? tab_get(g, id, 1) : NULL;
    if (r) {
        r->seen = 1;
        int att = json_get(ti, "attached", &a) && json_bool(a);
        if (r->ours) { if (!att) r->ours = 0; }
        else if (r->hib && att && !r->attached) r->want = 1;   /* a client came for it */
        r->attached = att;
        if (t >= g->quiet_until) r->last = t;
        /* someone else navigated it (our own about:blank navigation reports the old URL first) */
        if (r->hib && t >= g->quiet_until && url && strcmp(url, "about:blank")) r->hib = 0;
    }
    free(id); free(url);
}

static int restore_tab(gov_t *g, tabrec *t);
/* Sleep up to ms, handling target events when a persistent connection exists; a client attaching
 * to a hibernated tab restores it at once. */
static void pump(gov_t *g, int ms) {
    if (!(g->cfg->hibernate > 0 && g->cdp_open && g->cdp.open)) { sleep_ms(ms); return; }
    long long dl = now_ms() + ms;
    for (;;) {
        long long left = dl - now_ms();
        if (left <= 0 || g_stop) return;
        char *msg; jv pr, m;
        int r = cdp_event(&g->cdp, NULL, "Target.targetCreated|Target.targetInfoChanged|Target.targetDestroyed", (int)left, &msg, &pr, &m);
        if (r < 0) { cdp_done(g); sleep_ms((int)(dl - now_ms() > 0 ? dl - now_ms() : 0)); return; }
        if (r == 0) return;
        tab_event(g, m, pr);
        free(msg);
        for (int i = 0; i < g->ntabs; i++) if (g->tabs[i].want && g->tabs[i].hib) { restore_tab(g, &g->tabs[i]); break; }
    }
}

static int restore_tab(gov_t *g, tabrec *t) {
    char *sid = NULL, p[64];
    int ok = 0;
    t->ours = 1;
    if (cdp_attach(&g->cdp, t->id, 5000, &sid)) t->ours = 0;
    else {
        snprintf(p, sizeof p, "{\"entryId\":%lld}", t->entry);
        ok = cdp_call(&g->cdp, sid, "Page.navigateToHistoryEntry", p, 5000, NULL, NULL) == 0;
        cdp_detach(&g->cdp, sid, 2000);
    }
    free(sid);
    t->hib = t->want = 0; t->last = now_ms();
    g->quiet_until = now_ms() + 1500;
    ev_t e; ev_new(g, &e, "restore"); ev_s(&e, "tab", t->id); ev_i(&e, "ok", ok); ev_end(g, &e);
    return ok;
}

/* A client attached to a hibernated tab: bring its page back. */
static void restore_check(gov_t *g, int all) {
    if (g->cfg->hibernate <= 0 || (!all && now_ms() < g->quiet_until)) return;
    for (int i = 0; i < g->ntabs; i++)
        if (g->tabs[i].hib && (all || g->tabs[i].attached || g->tabs[i].want) && cdp_up(g) == 0) restore_tab(g, &g->tabs[i]);
}

static int hibernate_tab(gov_t *g, tabrec *t) {
    char *sid = NULL, *resp = NULL;
    jv res, ent;
    int ok = 0;
    t->ours = 1;
    if (cdp_attach(&g->cdp, t->id, 5000, &sid)) { t->ours = 0; return 0; }
    if (cdp_call(&g->cdp, sid, "Page.getNavigationHistory", NULL, 5000, &resp, &res) == 0) {
        char path[48];
        snprintf(path, sizeof path, "entries.%lld", (long long)json_get_num(res, "currentIndex", -1));
        char *url = json_get(res, path, &ent) ? json_get_str(ent, "url") : NULL;
        if (url && (!strncmp(url, "http", 4) || !strncmp(url, "file:", 5))) {
            t->entry = (long long)json_get_num(ent, "id", -1);
            if (t->entry >= 0 && cdp_call(&g->cdp, sid, "Page.navigate", "{\"url\":\"about:blank\"}", 5000, NULL, NULL) == 0) {
                t->hib = 1; ok = 1;
                ev_t e; ev_new(g, &e, "hibernate"); ev_s(&e, "tab", t->id); ev_s(&e, "url", url); ev_end(g, &e);
            }
        }
        free(url); free(resp);
    }
    cdp_detach(&g->cdp, sid, 2000);
    free(sid);
    return ok;
}

/* Hibernate page tabs beyond the K most recently active (untouched >= hib_min_s, no client). */
static int hibernate_pass(gov_t *g, int ignore_age) {
    int idx[MAXTABS], n = 0, done = 0;
    for (int i = 0; i < g->ntabs; i++) if (g->tabs[i].seen) idx[n++] = i;
    for (int i = 1; i < n; i++)    /* most recent first */
        for (int j = i; j > 0 && g->tabs[idx[j]].last > g->tabs[idx[j - 1]].last; j--) { int x = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = x; }
    long long t = now_ms();
    for (int i = g->cfg->hibernate; i < n; i++) {
        tabrec *r = &g->tabs[idx[i]];
        if (r->hib || r->attached || (!ignore_age && t - r->last < (long long)g->cfg->hib_min_s * 1000)) continue;
        done += hibernate_tab(g, r);
    }
    g->quiet_until = now_ms() + 1500;
    return done;
}

/* Memory.simulatePressureNotification critical on the browser and every page, then
 * HeapProfiler.collectGarbage on every page. (Never Memory.forciblyPurgeJavaScriptMemory: it
 * destroys the page's V8 contexts.) */
static void gc_pass(gov_t *g, slim_gov_pass *r) {
    static const char crit[] = "{\"level\":\"critical\"}";
    if (cdp_up(g)) { ev_msg(g, "no-cdp", "why", "DevTools not reachable"); return; }
    r->pressure = cdp_call(&g->cdp, NULL, "Memory.simulatePressureNotification", crit, 5000, NULL, NULL) == 0;
    char *resp; jv res, arr, t;
    size_t pos = 0;
    if (cdp_call(&g->cdp, NULL, "Target.getTargets", NULL, 5000, &resp, &res) == 0) {
        if (json_get(res, "targetInfos", &arr))
            while (json_next(arr, &pos, NULL, &t)) {
                if (!json_streq_path(t, "type", "page")) continue;
                char *id = json_get_str(t, "targetId"), *sid = NULL;
                tabrec *tr = id && g->cfg->hibernate > 0 ? tab_get(g, id, 1) : NULL;
                if (tr && !tr->seen) {   /* --once: no event history, take getTargets' view */
                    jv a;
                    tr->seen = 1; tr->attached = json_get(t, "attached", &a) && json_bool(a);
                }
                if (tr && !tr->hib) tr->ours = 1;
                if (id && !(tr && tr->hib) && cdp_attach(&g->cdp, id, 5000, &sid) == 0) {
                    cdp_call(&g->cdp, sid, "Memory.simulatePressureNotification", crit, 5000, NULL, NULL);
                    if (cdp_call(&g->cdp, sid, "HeapProfiler.collectGarbage", NULL, 10000, NULL, NULL) == 0) r->tabs_gc++;
                    cdp_detach(&g->cdp, sid, 2000);
                }
                free(sid); free(id);
            }
        free(resp);
    }
    g->quiet_until = now_ms() + 1500;
}

/* ---- passes ---- */
static void gov_init(gov_t *g, slim_gov_cfg *cfg) {
    memset(g, 0, sizeof *g);
    g->cfg = cfg;
    g->hz = sysconf(_SC_CLK_TCK);
    if (g->hz <= 0) g->hz = 100;
    if (!cfg->root_start) { pst s; if (pstat(cfg->root, &s) == 0) cfg->root_start = s.start; }
}

static void measure(gov_t *g, long *iso, long *anon, long *file) {
    slim_mem m;
    if (slim_mem_tree(g->cfg->root, &m)) { *iso = *anon = *file = -1; return; }
    *iso = m.iso_kb; *anon = m.anon_kb + m.shmem_kb; *file = m.file_union_kb;
}

static void do_pass(gov_t *g, slim_gov_pass *r, int gc, int hib, int rec, int ignore_age) {
    long long t0 = now_ms();
    memset(r, 0, sizeof *r);
    r->iso_before_kb = r->iso_after_kb = -1;
    if (g->cfg->measure) measure(g, &r->iso_before_kb, &r->anon_before_kb, &r->file_before_kb);
    if (gc && g->cfg->gc && g->cfg->port >= 0) gc_pass(g, r);
    if (hib && g->cfg->hibernate > 0 && g->cfg->port >= 0 && cdp_up(g) == 0) r->hibernated = hibernate_pass(g, ignore_age);
    if (rec && g->cfg->reclaim_s > 0 && !g->reclaim_off) reclaim(g, r);
    if (g->cfg->hibernate <= 0) cdp_done(g);   /* no connection held between passes */
    r->procs = g->n;
    if (g->cfg->measure) measure(g, &r->iso_after_kb, &r->anon_after_kb, &r->file_after_kb);
    r->ms = (long)(now_ms() - t0);
    ev_t e; ev_new(g, &e, "pass");
    ev_i(&e, "gc", gc); ev_i(&e, "reclaim", rec);
    if (g->cfg->measure) { ev_kb(&e, "iso_before", r->iso_before_kb); ev_kb(&e, "iso_after", r->iso_after_kb); ev_kb(&e, "file_union_after", r->file_after_kb); }
    if (gc) { ev_i(&e, "tabs_gc", r->tabs_gc); ev_i(&e, "pressure", r->pressure); }
    if (rec) { ev_kb(&e, "reclaimed", r->reclaimed_kb); ev_i(&e, "cgroups", r->cgroups); ev_i(&e, "cgroups_reclaimed", r->cgroups_ok); ev_i(&e, "foreign", r->foreign); }
    if (r->hibernated) ev_i(&e, "hibernated", r->hibernated);
    ev_i(&e, "procs", r->procs); ev_i(&e, "ms", r->ms);
    ev_end(g, &e);
}

int slim_gov_once(slim_gov_cfg *cfg, slim_gov_pass *r) {
    gov_t *g = calloc(1, sizeof *g);
    if (!g) return slim_seterr("out of memory");
    gov_init(g, cfg);
    if (!alive_as(cfg->root, cfg->root_start)) { free(g); return slim_seterr("browser pid %d is not running", (int)cfg->root); }
    scan_tree(g);
    slim_gov_pass a;
    do_pass(g, &a, 1, cfg->hibernate > 0, 0, 1);
    sleep_ms(1500);   /* let the GC/purge settle */
    do_pass(g, r, 0, 0, 1, 1);
    r->iso_before_kb = a.iso_before_kb; r->anon_before_kb = a.anon_before_kb; r->file_before_kb = a.file_before_kb;
    r->tabs_gc = a.tabs_gc; r->pressure = a.pressure; r->hibernated = a.hibernated; r->ms += a.ms + 1500;
    cdp_done(g);
    free(g);
    return 0;
}

int slim_gov_run(slim_gov_cfg *cfg) {
    gov_t *g = calloc(1, sizeof *g);
    if (!g) return slim_seterr("out of memory");
    gov_init(g, cfg);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    if (setpriority(PRIO_PROCESS, 0, 10)) { /* best effort */ }
    if (cfg->wait_engine) {   /* root is still slimium-chrome (or systemd-run) until it execs the engine */
        char self[PATH_MAX], cur[PATH_MAX], p[48];
        ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
        self[sl > 0 ? sl : 0] = 0;
        snprintf(p, sizeof p, "/proc/%d/exe", (int)cfg->root);
        for (long long dl = now_ms() + 60000; now_ms() < dl && alive_as(cfg->root, cfg->root_start) && !g_stop; sleep_ms(50)) {
            ssize_t l = readlink(p, cur, sizeof cur - 1);
            if (l <= 0) continue;
            cur[l] = 0;
            if (strcmp(cur, self) && strcmp(path_base(cur), "systemd-run")) break;
        }
    }
    ev_t e; ev_new(g, &e, "start");
    ev_i(&e, "port", cfg->port); ev_i(&e, "idle_ms", cfg->idle_ms); ev_d(&e, "idle_cpu", cfg->idle_cpu);
    ev_i(&e, "reclaim_s", cfg->reclaim_s); ev_i(&e, "grow_mb", cfg->grow_mb); ev_i(&e, "min_s", cfg->min_s); ev_i(&e, "gc", cfg->gc); ev_i(&e, "hibernate", cfg->hibernate);
    { char c[512]; ev_s(&e, "cgroup", cg_of(cfg->root, c, sizeof c) ? NULL : c); }
    ev_end(g, &e);
    /* busy_gc / busy_rec: CPU time (ms) the tree spent above idle_cpu since the last GC pass /
     * reclaim. Chromium's own background blips (timers, refaults after a reclaim) stay below
     * busy_ms, so they neither re-run the GC pass nor pull the next reclaim forward. */
    long long tprev = now_ms(), idle_since = now_ms(), last_reclaim = 0, grace_until = 0;
    double busy_gc = 1e9, busy_rec = 1e9;
    int idle = 0;
    scan_tree(g);
    sample_cpu(g, &tprev);
    while (!g_stop) {
        long long t0 = now_ms();
        pump(g, 1000);
        if (!alive_as(cfg->root, cfg->root_start)) break;
        long long t = now_ms();
        if (t - g->last_scan >= SCAN_MS) scan_tree(g);
        double cpu = sample_cpu(g, &tprev);
        restore_check(g, 0);
        if (t < grace_until) continue;
        if (cpu >= cfg->idle_cpu) {
            double ms = cpu / 100.0 * (double)(t - t0);
            busy_gc += ms; busy_rec += ms;
            if (idle) { ev_new(g, &e, "active"); ev_d(&e, "cpu", cpu); ev_end(g, &e); }
            idle_since = t; idle = 0;
            continue;
        }
        if (t - idle_since < cfg->idle_ms) continue;
        if (!idle) { idle = 1; ev_new(g, &e, "idle"); ev_d(&e, "cpu", cpu); ev_i(&e, "busy_ms", (long long)(busy_gc < 1e8 ? busy_gc : -1)); ev_end(g, &e); }
        int gc = busy_gc >= cfg->busy_ms && cfg->port >= 0 && (cfg->gc || cfg->hibernate > 0);
        /* reclaim on a later tick than the GC pass, once its CPU has settled (grace) */
        int rec = !gc && cfg->reclaim_s > 0 && !g->reclaim_off &&
                  (busy_rec >= cfg->busy_ms || t - last_reclaim >= (long long)cfg->reclaim_s * 1000);
        long grown = -1;
        if (!gc && !rec && cfg->reclaim_s > 0 && !g->reclaim_off && cfg->grow_mb > 0 &&
            t - last_reclaim >= (long long)cfg->min_s * 1000 && (grown = file_growth(g)) >= (long)cfg->grow_mb * 1024)
            rec = 2;   /* adaptive: the page cache came back */
        if (!gc && !rec) continue;
        slim_gov_pass r;
        do_pass(g, &r, gc, gc, rec, 0);
        if (gc) busy_gc = 0;
        if (rec) { busy_rec = 0; last_reclaim = now_ms(); }
        if (rec == 2) { ev_new(g, &e, "grown"); ev_kb(&e, "file_growth", grown); ev_end(g, &e); }
        grace_until = now_ms() + (gc ? 2000 : GRACE_MS);
    }
    if (g_stop && alive_as(cfg->root, cfg->root_start)) restore_check(g, 1);
    ev_new(g, &e, "exit");
    ev_s(&e, "why", g_stop ? "signal" : "browser exited");
    ev_end(g, &e);
    cdp_done(g);
    free(g);
    return 0;
}

/* ---- configuration ---- */
static long envl(const char *k, long def, long lo, long hi) {
    const char *v = getenv(k);
    long n;
    if (!v || !*v || parse_int(v, &n) || n < lo || n > hi) return def;
    return n;
}

void slim_gov_defaults(slim_gov_cfg *g) {
    memset(g, 0, sizeof *g);
    g->port = -1;
    g->idle_ms = (int)envl("SLIM_GOV_IDLE_MS", 5000, 0, 86400000);
    g->reclaim_s = (int)envl("SLIM_GOV_RECLAIM_S", 30, 0, 86400);
    g->gc = (int)envl("SLIM_GOV_GC", 1, 0, 1);
    g->hibernate = (int)envl("SLIM_GOV_HIBERNATE", 0, 0, 1000);
    g->hib_min_s = (int)envl("SLIM_GOV_HIBERNATE_MIN_S", 120, 0, 86400 * 30);
    g->busy_ms = (int)envl("SLIM_GOV_BUSY_MS", 1000, 0, 86400000);
    g->grow_mb = (int)envl("SLIM_GOV_RECLAIM_GROW_MB", 24, 0, 1 << 20);
    g->min_s = (int)envl("SLIM_GOV_RECLAIM_MIN_S", 5, 0, 86400);
    g->idle_cpu = 5;
    const char *c = getenv("SLIM_GOV_IDLE_CPU");
    double d;
    if (c && *c && !parse_double(c, &d) && d > 0 && d <= 10000) g->idle_cpu = d;
    g->out_fd = g->log_fd = -1;
    const char *lf = getenv("SLIM_GOV_LOG");
    if (lf && *lf) g->log_fd = open(lf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

/* ---- launch helpers ---- */
int slim_gov_argv_port(int argc, char **argv, const char **udd) {
    int port = -1;
    *udd = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) break;
        long n;
        if (!strncmp(a, "--remote-debugging-port=", 24) && !parse_int(a + 24, &n) && n >= 0 && n < 65536) port = (int)n;
        else if (!strncmp(a, "--user-data-dir=", 16)) *udd = a + 16;
    }
    return port;
}

pid_t slim_gov_spawn(pid_t root, int port, const char *udd, int wait_engine) {
    char self[PATH_MAX], a_pid[24], a_start[24], a_port[24];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return -1;
    self[l] = 0;
    if (access(self, X_OK)) snprintf(self, sizeof self, "/proc/self/exe");   /* replaced binary: "(deleted)" */
    pst s;
    if (pstat(root, &s)) return -1;
    snprintf(a_pid, sizeof a_pid, "%d", (int)root);
    snprintf(a_start, sizeof a_start, "%llu", s.start);
    if (port < 0) snprintf(a_port, sizeof a_port, "none"); else snprintf(a_port, sizeof a_port, "%d", port);
    char *av[16];
    int n = 0;
    av[n++] = "slimium"; av[n++] = "gov"; av[n++] = "--daemon";
    av[n++] = "--pid"; av[n++] = a_pid; av[n++] = "--pid-start"; av[n++] = a_start;
    av[n++] = "--cdp"; av[n++] = a_port;
    if (udd && *udd) { av[n++] = "--udd"; av[n++] = (char *)udd; }
    if (wait_engine) av[n++] = "--wait-engine";
    av[n] = NULL;
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) return -1;
    pid_t c = fork();
    if (c < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (c == 0) {
        pid_t gc = fork();
        if (gc != 0) { ssize_t w = write(pfd[1], &gc, sizeof gc); (void)w; _exit(0); }
        int nul = open("/dev/null", O_RDWR);     /* first: release the caller's pipes */
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        setsid();
        signal(SIGHUP, SIG_IGN);
        closefrom(3);
        execv(self, av);
        _exit(127);
    }
    close(pfd[1]);
    pid_t got = -1;
    if (read(pfd[0], &got, sizeof got) != (ssize_t)sizeof got) got = -1;
    close(pfd[0]);
    while (waitpid(c, NULL, 0) < 0 && errno == EINTR) {}
    return got;
}

char **slim_scope_argv(char **av, pid_t pid) {
    const char *e = getenv("SLIM_SCOPE");
    if (e && !strcmp(e, "0")) return NULL;
    char cg[512];
    if (cg_of(getpid(), cg, sizeof cg)) return NULL;         /* no cgroup v2 */
    if (strstr(cg, "/slimium-")) return NULL;                /* already in one of ours */
    char *sr = find_in_path("systemd-run");
    if (!sr) return NULL;
    pst s;
    unsigned long long st = pstat(pid, &s) == 0 ? s.start : 0;
    char probe[64];
    snprintf(probe, sizeof probe, "--unit=slimium-probe-%d-%llu", (int)pid, st);
    pid_t c = fork();
    if (c < 0) { free(sr); return NULL; }
    if (c == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        char *pa[] = {sr, "--user", "--scope", "--quiet", "--collect", probe, "--", "true", NULL};
        execv(sr, pa);
        _exit(127);
    }
    int status = -1, done = 0;
    for (long long dl = now_ms() + 3000; now_ms() < dl;) {
        pid_t w = waitpid(c, &status, WNOHANG);
        if (w == c) { done = 1; break; }
        if (w < 0 && errno != EINTR) break;
        sleep_ms(2);
    }
    if (!done) { kill(c, SIGKILL); waitpid(c, NULL, 0); free(sr); return NULL; }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { free(sr); return NULL; }
    int n = 0;
    while (av[n]) n++;
    size_t srl = strlen(sr) + 1, unl = 64;
    char **out = malloc(sizeof *out * (size_t)(n + 8) + srl + unl);
    if (!out) { free(sr); return NULL; }
    char *str = (char *)(out + n + 8), *unit = str + srl;
    memcpy(str, sr, srl);
    free(sr);
    snprintf(unit, unl, "--unit=slimium-%d-%llu", (int)pid, st);
    int k = 0;
    out[k++] = str; out[k++] = "--user"; out[k++] = "--scope"; out[k++] = "--quiet"; out[k++] = "--collect";
    out[k++] = unit; out[k++] = "--";
    for (int i = 0; i < n; i++) out[k++] = av[i];
    out[k] = NULL;
    return out;
}

static int advise(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    close(fd);
    return ok;
}

static int advise_dir(const char *dir, int any) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int n = 0;
    char p[PATH_MAX];
    while ((de = readdir(d))) {
        const char *x = strrchr(de->d_name, '.');
        if (de->d_name[0] == '.' || !x) continue;
        if (!any && strcmp(x, ".so") && strcmp(x, ".pak") && strcmp(x, ".dat") && strcmp(x, ".bin")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) < (int)sizeof p) n += advise(p);
    }
    closedir(d);
    return n;
}

int slim_engine_fadvise(const char *engine) {
    const char *e = getenv("SLIM_GOV_FADVISE");
    if ((e && !strcmp(e, "0")) || !engine) return 0;
    char rp[PATH_MAX], dir[PATH_MAX];
    if (!realpath(engine, rp)) return 0;
    int n = advise(rp);
    snprintf(dir, sizeof dir, "%s", rp);
    char *sl = strrchr(dir, '/');
    if (!sl || sl == dir) return n;
    *sl = 0;
    n += advise_dir(dir, 0);
    size_t l = strlen(dir);
    if (l + 9 < sizeof dir) { memcpy(dir + l, "/locales", 9); n += advise_dir(dir, 0); }
    return n;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    struct stat st;
    int out = fstat(in, &st) ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (out < 0) { close(in); return -1; }
    off_t left = st.st_size;
    while (left > 0) {   /* reflink on btrfs/xfs, in-kernel copy elsewhere */
        ssize_t n = copy_file_range(in, NULL, out, NULL, (size_t)(left > (1 << 30) ? (1 << 30) : left), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        left -= n;
    }
    if (left > 0 && lseek(in, st.st_size - left, SEEK_SET) >= 0) {   /* EXDEV/ENOSYS: plain copy */
        char *buf = malloc(1 << 16);
        while (buf && left > 0) {
            ssize_t n = read(in, buf, 1 << 16);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            ssize_t o = 0;
            while (o < n) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0 && errno == EINTR) continue; if (w <= 0) break; o += w; }
            if (o < n) break;
            left -= n;
        }
        free(buf);
    }
    int ok = left == 0 && fsync(out) == 0;
    if (ok) posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);   /* written pages were charged to us, the launcher */
    close(out); close(in);
    return ok ? 0 : -1;
}

/* Remove a copy dir made by slim_engine_private (regular files and symlinks only, then the dir). */
static void rm_copy(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    char p[PATH_MAX];
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) < (int)sizeof p) unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

char *slim_engine_private(const char *engine) {
    const char *e = getenv("SLIM_ENGINE_COPY");
    if ((e && !strcmp(e, "0")) || !engine) return NULL;
    char src[PATH_MAX], sdir[1024], root[1024], key[256], fin[1400], tmp[1400], p[2048], q[2048];
    struct stat st, s2;
    if (!realpath(engine, src) || stat(src, &st) || !S_ISREG(st.st_mode) || strlen(src) >= sizeof sdir) return NULL;
    memcpy(sdir, src, strlen(src) + 1);
    char *sl = strrchr(sdir, '/');
    if (!sl || sl == sdir) return NULL;
    *sl = 0;
    const char *base = sl + 1 - sdir + src;
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x == '/') snprintf(root, sizeof root, "%s/slimium/engines", x);
    else if (h && *h == '/') snprintf(root, sizeof root, "%s/.cache/slimium/engines", h);
    else return NULL;
    if (strstr(src, root) == src) return NULL;   /* already a copy */
    snprintf(key, sizeof key, "%.64s-%lx-%lx-%lld-%lld", base, (unsigned long)st.st_dev, (unsigned long)st.st_ino,
             (long long)st.st_size, (long long)st.st_mtime);
    snprintf(fin, sizeof fin, "%s/%s", root, key);
    snprintf(p, sizeof p, "%s/%s", fin, base);
    snprintf(q, sizeof q, "%s/.ok", fin);
    if (stat(q, &s2) == 0 && stat(p, &s2) == 0 && s2.st_size == st.st_size) return xstrdup(p);
    if (mkdir_p(root, 0700)) return NULL;
    snprintf(tmp, sizeof tmp, "%s/.tmp-%d", root, (int)getpid());
    rm_copy(tmp);
    if (mkdir(tmp, 0700)) return NULL;
    DIR *d = opendir(sdir);
    int bad = !d;
    struct dirent *de;
    while (!bad && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", tmp, de->d_name);
        snprintf(q, sizeof q, "%s/%s", sdir, de->d_name);
        if (!strcmp(de->d_name, base)) bad = copy_file(q, p, 0755) != 0;
        else bad = symlink(q, p) != 0;
    }
    if (d) closedir(d);
    snprintf(p, sizeof p, "%s/.ok", tmp);
    int okfd = bad ? -1 : open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (okfd < 0) { rm_copy(tmp); return NULL; }
    close(okfd);
    if (rename(tmp, fin)) rm_copy(tmp);    /* lost a race: the other copy is as good */
    snprintf(q, sizeof q, "%s/.ok", fin);
    snprintf(p, sizeof p, "%s/%s", fin, base);
    if (stat(q, &s2) || stat(p, &s2) || s2.st_size != st.st_size) return NULL;
    /* drop copies of older versions of this engine (a running browser keeps its unlinked inode) */
    size_t bl = strlen(base);
    if ((d = opendir(root))) {
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, base, bl) || de->d_name[bl] != '-' || !strcmp(de->d_name, key)) continue;
            if (snprintf(q, sizeof q, "%s/%s", root, de->d_name) < (int)sizeof q) rm_copy(q);
        }
        closedir(d);
    }
    return xstrdup(p);
}

pid_t slim_gov_find(pid_t root) {
    char want[24];
    snprintf(want, sizeof want, "%d", (int)root);
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    pid_t found = 0;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        char p[64], b[1024];
        snprintf(p, sizeof p, "/proc/%.24s/cmdline", de->d_name);
        long n = rdb(p, b, sizeof b);
        if (n <= 0) continue;
        /* argv: slimium gov ... --pid ROOT ... */
        const char *a[24]; int k = 0;
        for (long i = 0; i < n && k < 24; i += (long)strlen(b + i) + 1) a[k++] = b + i;
        if (k < 4 || strcmp(path_base(a[0]), "slimium") || strcmp(a[1], "gov")) continue;
        for (int i = 2; i + 1 < k; i++) if (!strcmp(a[i], "--pid") && !strcmp(a[i + 1], want)) found = (pid_t)atoi(de->d_name);
    }
    closedir(d);
    return found;
}
