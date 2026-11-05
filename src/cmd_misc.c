/* Memory reporting/purging and raw DevTools passthrough. */
#include "cmd.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int browser_pid(ctx_t *c) {
    if (ctx_state(c)) return 0;
    if (c->have_st && c->st.pid > 0 && !c->st.attached) return c->st.pid;
    return find_browser_pid(c->port ? c->port : c->st.port);
}

/* For every page target call fn with a fresh session. */
typedef int (*tab_fn)(ctx_t *c, const char *sid, jv info, void *arg);
static int each_tab(ctx_t *c, tab_fn fn, void *arg) {
    char *resp; jv arr, t; size_t pos = 0;
    if (ctx_targets(c, &resp, &arr)) return -1;
    while (json_next(arr, &pos, NULL, &t)) {
        if (!json_streq_path(t, "type", "page")) continue;
        char *id = json_get_str(t, "targetId"), *sid = NULL;
        if (id && cdp_attach(&c->cdp, id, c->timeout, &sid) == 0) {
            fn(c, sid, t, arg);
            cdp_detach(&c->cdp, sid, 2000);
        }
        free(sid); free(id);
    }
    free(resp);
    return 0;
}

static int heap_tab(ctx_t *c, const char *sid, jv info, void *arg) {
    int *n = arg;
    char *resp; jv res;
    int to = c->timeout < 5000 ? c->timeout : 5000;
    char *id = json_get_str(info, "targetId"), *u = json_get_str(info, "url");
    double used = -1, total = -1;
    if (cdp_call(&c->cdp, sid, "Runtime.getHeapUsage", NULL, to, &resp, &res) == 0) {
        used = json_get_num(res, "usedSize", -1); total = json_get_num(res, "totalSize", -1);
        free(resp);
    }
    if ((*n)++) sb_putc(&c->oj, ',');
    sb_puts(&c->oj, "{\"id\":"); sb_jstrz(&c->oj, id);
    sb_puts(&c->oj, ",\"url\":"); sb_jstrz(&c->oj, u);
    sb_printf(&c->oj, ",\"heap_used_kb\":%.0f,\"heap_total_kb\":%.0f}", used >= 0 ? used / 1024 : -1, total >= 0 ? total / 1024 : -1);
    out_t(c, "\ntab %s heap %.0f/%.0f KB %s", id ? id : "", used / 1024, total / 1024, u ? u : "");
    free(id); free(u);
    return 0;
}

static void mem_line(ctx_t *c, int pid, const char *prefix) {
    slim_mem m; memset(&m, 0, sizeof m);
    if (pid > 0 && slim_mem_tree(pid, &m) == 0) {
        char k[48];
        snprintf(k, sizeof k, "%sprocs", prefix); out_i(c, k, m.procs);
        snprintf(k, sizeof k, "%spss_kb", prefix); out_i(c, k, m.pss_kb);
        snprintf(k, sizeof k, "%srss_kb", prefix); out_i(c, k, m.rss_kb);
        snprintf(k, sizeof k, "%suss_kb", prefix); out_i(c, k, m.uss_kb);
        out_t(c, "%sbrowser pid %d procs %d pss %ld KB rss %ld KB uss %ld KB", prefix, pid, m.procs, m.pss_kb, m.rss_kb, m.uss_kb);
    } else out_t(c, "%sbrowser pid %d (memory unavailable)", prefix, pid);
}

int cmd_mem(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "mem takes no arguments");
    if (ctx_browser(c)) return -1;
    int pid = browser_pid(c);
    out_i(c, "pid", pid);
    mem_line(c, pid, "");
    int n = 0;
    sb_puts(&c->oj, ",\"tabs\":[");
    int r = each_tab(c, heap_tab, &n);
    sb_putc(&c->oj, ']');
    return r;
}

static int purge_tab(ctx_t *c, const char *sid, jv info, void *arg) {
    (void)info;
    int *n = arg;
    int to = c->timeout < 10000 ? c->timeout : 10000;
    long a, b;
    if (cdp_send(&c->cdp, sid, "HeapProfiler.collectGarbage", NULL, &a) == 0 &&
        cdp_send(&c->cdp, sid, "Memory.forciblyPurgeJavaScriptMemory", NULL, &b) == 0) {
        int ok = cdp_wait(&c->cdp, a, sid, to, NULL, NULL) == 0;
        cdp_wait(&c->cdp, b, sid, to, NULL, NULL);
        if (ok) (*n)++;
    }
    return 0;
}

int cmd_purge(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "purge takes no arguments");
    if (ctx_browser(c)) return -1;
    int pid = browser_pid(c);
    slim_mem before; memset(&before, 0, sizeof before);
    int hb = pid > 0 && slim_mem_tree(pid, &before) == 0;
    int n = 0;
    if (each_tab(c, purge_tab, &n)) return -1;
    int pressure = cdp_call(&c->cdp, NULL, "Memory.simulatePressureNotification", "{\"level\":\"critical\"}", c->timeout, NULL, NULL) == 0;
    sleep_ms(300);
    slim_mem after; memset(&after, 0, sizeof after);
    int ha = hb && slim_mem_tree(pid, &after) == 0;
    out_i(c, "tabs_gc", n); out_b(c, "pressure", pressure);
    out_t(c, "gc on %d tab%s, pressure %s", n, n == 1 ? "" : "s", pressure ? "sent" : "unavailable");
    if (ha) {
        out_i(c, "pss_before_kb", before.pss_kb); out_i(c, "pss_after_kb", after.pss_kb);
        out_t(c, ", pss %ld -> %ld KB", before.pss_kb, after.pss_kb);
    }
    return 0;
}

static int browser_domain(const char *m) {
    static const char *const d[] = {"Target.", "Browser.", "SystemInfo.", "Tethering.", "Tracing.", NULL};
    for (int i = 0; d[i]; i++) if (!strncmp(m, d[i], strlen(d[i]))) return 1;
    return 0;
}

int cmd_cdp(ctx_t *c, int argc, char **argv) {
    const char *tgt = opt_val(c, &argc, argv, "--target");
    int ev_ms = opt_ms(c, &argc, argv, "--events", 0);
    if (opt_done(c, &argc, argv)) return -1;
    if (argc < 1 || argc > 2 || !strchr(argv[0], '.')) return usage_err(c, "cdp Domain.method [PARAMS_JSON] [--target browser|page] [--events MS]");
    int page;
    if (!tgt) page = !browser_domain(argv[0]);
    else if (!strcmp(tgt, "browser")) page = 0;
    else if (!strcmp(tgt, "page")) page = 1;
    else return usage_err(c, "bad --target '%s' (browser|page)", tgt);
    const char *params = argc == 2 ? argv[1] : NULL;
    if (params) {
        jv d;
        if (json_parse(params, strlen(params), &d) || d.t != J_OBJ) return usage_err(c, "PARAMS must be a JSON object");
    }
    char *resp; jv res;
    if (ctx_call(c, page, argv[0], params, &resp, &res)) return -1;
    out_raw(c, "result", res.p, res.n);
    sb_add(&c->ot, res.p, res.n);
    free(resp);
    if (ev_ms > 0) {
        long long dl = now_ms() + ev_ms;
        int n = 0;
        sb_puts(&c->oj, ",\"events\":[");
        for (;;) {
            long long left = dl - now_ms();
            if (left <= 0) break;
            char *m; jv pr, mm;
            int r = cdp_event(&c->cdp, "*", NULL, (int)left, &m, &pr, &mm);
            if (r <= 0) break;
            if (n++) sb_putc(&c->oj, ',');
            sb_puts(&c->oj, "{\"method\":"); sb_add(&c->oj, mm.p, mm.n);
            sb_puts(&c->oj, ",\"params\":"); sb_add(&c->oj, pr.p, pr.n); sb_putc(&c->oj, '}');
            out_t(c, "\n");
            sb_add(&c->ot, mm.p + 1, mm.n - 2); sb_putc(&c->ot, ' '); sb_add(&c->ot, pr.p, pr.n);
            free(m);
        }
        sb_putc(&c->oj, ']');
    }
    return 0;
}
