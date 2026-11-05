/* Context, option parsing, output and connection helpers shared by commands. */
#include "cmd.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int usage_err(ctx_t *c, const char *fmt, ...) {
    char tmp[400];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    c->usage = 1;
    return slim_seterr("%s", tmp);
}

static void rm_args(int *argc, char **argv, int i, int k) {
    for (int j = i; j + k <= *argc; j++) argv[j] = argv[j + k];
    *argc -= k;
}

const char *opt_val(ctx_t *c, int *argc, char **argv, const char *name) {
    size_t nl = strlen(name);
    const char *v = NULL;
    for (int i = 0; i < *argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        if (!strncmp(argv[i], name, nl) && argv[i][nl] == '=') { v = argv[i] + nl + 1; rm_args(argc, argv, i, 1); i--; continue; }
        if (!strcmp(argv[i], name)) {
            if (i + 1 >= *argc) { usage_err(c, "option %s needs a value", name); return NULL; }
            v = argv[i + 1]; rm_args(argc, argv, i, 2); i--;
        }
    }
    return v;
}
int opt_flag(int *argc, char **argv, const char *name) {
    int f = 0;
    for (int i = 0; i < *argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        if (!strcmp(argv[i], name)) { f = 1; rm_args(argc, argv, i, 1); i--; }
    }
    return f;
}
int opt_done(ctx_t *c, int *argc, char **argv) {
    if (c->usage) return -1;
    for (int i = 0; i < *argc; i++) {
        if (!strcmp(argv[i], "--")) { rm_args(argc, argv, i, 1); break; }
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2]) return usage_err(c, "unknown option %s", argv[i]);
    }
    return 0;
}
int opt_ms(ctx_t *c, int *argc, char **argv, const char *name, int def) {
    const char *v = opt_val(c, argc, argv, name);
    long n;
    if (!v) return def;
    if (parse_int(v, &n) || n < 0 || n > 86400000) { usage_err(c, "bad value for %s: %s", name, v); return def; }
    return (int)n;
}

/* ---- output ---- */
static void key(ctx_t *c, const char *k) { sb_putc(&c->oj, ','); sb_jstrz(&c->oj, k); sb_putc(&c->oj, ':'); }
void out_s(ctx_t *c, const char *k, const char *v) { key(c, k); sb_jstrz(&c->oj, v); }
void out_sn(ctx_t *c, const char *k, const char *v, size_t n) { key(c, k); sb_jstr(&c->oj, v, n); }
void out_i(ctx_t *c, const char *k, long long v) { key(c, k); sb_printf(&c->oj, "%lld", v); }
void out_d(ctx_t *c, const char *k, double v) { key(c, k); sb_printf(&c->oj, "%.10g", v); }
void out_b(ctx_t *c, const char *k, int v) { key(c, k); sb_puts(&c->oj, v ? "true" : "false"); }
void out_raw(ctx_t *c, const char *k, const char *j, size_t n) { key(c, k); if (n) sb_add(&c->oj, j, n); else sb_puts(&c->oj, "null"); }
void out_t(ctx_t *c, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) { sb_add(&c->ot, tmp, (size_t)k); return; }
    sb_reserve(&c->ot, (size_t)k);
    va_start(ap, fmt);
    vsnprintf(c->ot.p + c->ot.n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    c->ot.n += (size_t)k;
}

/* ---- connection ---- */
int ctx_state(ctx_t *c) {
    if (c->port || c->have_st) return 0;
    if (state_load(c->session, &c->st)) return -1;
    c->have_st = 1;
    return 0;
}

int ctx_browser(ctx_t *c) {
    if (c->connected && c->cdp.open) return 0;
    if (c->connected) ctx_disconnect(c);
    if (ctx_state(c)) return -1;
    int port = c->port ? c->port : c->st.port;
    char *url = NULL;
    int ctimeout = c->timeout < 5000 ? c->timeout : 5000;
    if (c->have_st && c->st.ws && cdp_connect(&c->cdp, c->st.ws, ctimeout) == 0) {
        c->connected = 1;
    } else {
        if (cdp_browser_url(port, ctimeout, &url, NULL)) {
            if (c->have_st) return slim_seterr("browser of session '%s' is not running (port %d)", c->session, port);
            return -1;
        }
        if (cdp_connect(&c->cdp, url, ctimeout)) { free(url); return -1; }
        c->connected = 1;
        if (c->have_st) { state_set(&c->st.ws, url); state_save(&c->st); }
        free(url);
    }
    if (c->have_st && c->st.dldir) {
        sbuf p = {0};
        sb_puts(&p, "{\"behavior\":\"allow\",\"downloadPath\":"); sb_jstrz(&p, c->st.dldir); sb_putc(&p, '}');
        cdp_call(&c->cdp, NULL, "Browser.setDownloadBehavior", p.p, c->timeout, NULL, NULL);
        sb_free(&p);
    }
    return 0;
}

void ctx_reset_page(ctx_t *c) {
    if (c->sid && c->connected) cdp_detach(&c->cdp, c->sid, 2000);
    free(c->sid); c->sid = NULL;
    free(c->tid); c->tid = NULL;
    c->page_on = c->net_on = c->rt_on = 0;
}

void ctx_disconnect(ctx_t *c) {
    free(c->sid); c->sid = NULL;
    free(c->tid); c->tid = NULL;
    c->page_on = c->net_on = c->rt_on = 0;
    if (c->connected) cdp_close(&c->cdp);
    c->connected = 0;
}

int ctx_set_target(ctx_t *c, const char *tid) {
    if (c->have_st && (!c->st.target || strcmp(c->st.target, tid))) {
        state_set(&c->st.target, tid);
        return state_save(&c->st);
    }
    return 0;
}

int ctx_targets(ctx_t *c, char **resp, jv *arr) {
    jv res;
    if (ctx_browser(c)) return -1;
    if (cdp_call(&c->cdp, NULL, "Target.getTargets", NULL, c->timeout, resp, &res)) return -1;
    if (!json_get(res, "targetInfos", arr)) { free(*resp); return slim_seterr("Target.getTargets: bad response"); }
    return 0;
}

static int is_page(jv t) {
    if (!json_streq_path(t, "type", "page")) return 0;
    jv u;
    if (json_get(t, "url", &u) && u.n > 12 && !memcmp(u.p, "\"devtools://", 12)) return 0;
    return 1;
}

int ctx_resolve_target(ctx_t *c, const char *prefix, char **tid) {
    char *resp; jv arr, t; size_t pos = 0;
    if (ctx_targets(c, &resp, &arr)) return -1;
    size_t pl = strlen(prefix);
    char *found = NULL; int n = 0;
    while (json_next(arr, &pos, NULL, &t)) {
        char *id = json_get_str(t, "targetId");
        if (!id) continue;
        if (!strcasecmp(id, prefix)) { free(found); found = id; n = 1; break; }
        if (pl >= 1 && !strncasecmp(id, prefix, pl)) { n++; free(found); found = id; }
        else free(id);
    }
    free(resp);
    if (n == 0) { free(found); return slim_seterr("no tab with id '%s'", prefix); }
    if (n > 1) { free(found); return slim_seterr("tab id prefix '%s' is ambiguous", prefix); }
    *tid = found;
    return 0;
}

static int apply_emulation(ctx_t *c) {
    if (!c->have_st) return 0;
    slim_state *s = &c->st;
    sbuf p = {0};
    long ids[4]; int n = 0;
    if (s->vp_w > 0) {
        sb_printf(&p, "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%g,\"mobile\":%s}", s->vp_w, s->vp_h, s->vp_scale, s->vp_mobile ? "true" : "false");
        if (cdp_send(&c->cdp, c->sid, "Emulation.setDeviceMetricsOverride", p.p, &ids[n++])) goto fail;
    }
    if (s->ua) {
        sb_reset(&p); sb_puts(&p, "{\"userAgent\":"); sb_jstrz(&p, s->ua); sb_putc(&p, '}');
        if (cdp_send(&c->cdp, c->sid, "Emulation.setUserAgentOverride", p.p, &ids[n++])) goto fail;
    }
    if (s->offline || s->block) {
        if (ctx_enable(c, "Network")) goto fail;
    }
    if (s->offline) {
        if (cdp_send(&c->cdp, c->sid, "Network.emulateNetworkConditions",
                     "{\"offline\":true,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}", &ids[n++])) goto fail;
    }
    if (s->block) {
        sb_reset(&p); sb_puts(&p, "{\"urls\":"); sb_puts(&p, s->block); sb_putc(&p, '}');
        if (cdp_send(&c->cdp, c->sid, "Network.setBlockedURLs", p.p, &ids[n++])) goto fail;
    }
    for (int i = 0; i < n; i++)
        if (cdp_wait(&c->cdp, ids[i], c->sid, c->timeout, NULL, NULL)) goto fail;
    sb_free(&p);
    return 0;
fail:
    sb_free(&p);
    return slim_seterr("re-applying emulation: %s", slim_errmsg());
}

int ctx_page(ctx_t *c) {
    if (c->sid) return 0;
    if (ctx_browser(c)) return -1;
    char *want = NULL;
    if (c->tab) { if (ctx_resolve_target(c, c->tab, &want)) return -1; }
    else if (c->have_st && c->st.target) want = xstrdup(c->st.target);
    if (want && cdp_attach(&c->cdp, want, c->timeout, &c->sid)) {
        if (c->tab) { free(want); return -1; }
        free(want); want = NULL; /* stale current tab: fall back */
    }
    if (!want) {
        char *resp; jv arr, t; size_t pos = 0;
        if (ctx_targets(c, &resp, &arr)) return -1;
        while (json_next(arr, &pos, NULL, &t)) if (is_page(t)) { want = json_get_str(t, "targetId"); break; }
        free(resp);
        if (!want) {
            jv res;
            if (cdp_call(&c->cdp, NULL, "Target.createTarget", "{\"url\":\"about:blank\"}", c->timeout, &resp, &res)) return -1;
            want = json_get_str(res, "targetId");
            free(resp);
            if (!want) return slim_seterr("could not create a tab");
        }
        if (cdp_attach(&c->cdp, want, c->timeout, &c->sid)) { free(want); return -1; }
    }
    c->tid = want;
    if (!c->tab) ctx_set_target(c, want);
    return apply_emulation(c);
}

int ctx_call(ctx_t *c, int page, const char *method, const char *params, char **resp, jv *res) {
    if (page ? ctx_page(c) : ctx_browser(c)) return -1;
    return cdp_call(&c->cdp, page ? c->sid : NULL, method, params, c->timeout, resp, res);
}

int ctx_enable(ctx_t *c, const char *domain) {
    int *flag = !strcmp(domain, "Page") ? &c->page_on : !strcmp(domain, "Network") ? &c->net_on : &c->rt_on;
    if (*flag) return 0;
    if (ctx_page(c)) return -1;
    char m[64]; snprintf(m, sizeof m, "%s.enable", domain);
    if (cdp_call(&c->cdp, c->sid, m, NULL, c->timeout, NULL, NULL)) return -1;
    *flag = 1;
    return 0;
}

static int exception_err(jv res) {
    jv ex;
    if (!json_get(res, "exceptionDetails", &ex)) return 0;
    char *d = json_get_str(ex, "exception.description");
    if (!d) d = json_get_str(ex, "exception.value");
    if (!d) d = json_get_str(ex, "text");
    char *nl = d ? strchr(d, '\n') : NULL;
    if (nl) *nl = 0;
    const char *m = d ? d : "JavaScript exception";
    if (!strncmp(m, "Error: ", 7)) m += 7;
    const char *q = strstr(m, "' is not a valid selector");
    if (q && strstr(m, "SyntaxError")) {
        const char *st = q;
        while (st > m && !(st[-1] == '\'' && st - 1 > m && st[-2] == ' ')) st--;
        slim_seterr("bad selector '%.*s'", (int)(q - st), st);
    } else slim_seterr("%s", m);
    free(d);
    return -1;
}

int ctx_eval_in(ctx_t *c, const char *sid, const char *expr, char **resp, jv *robj) {
    sbuf p = {0};
    sb_puts(&p, "{\"expression\":"); sb_jstrz(&p, expr);
    sb_puts(&p, ",\"returnByValue\":true,\"awaitPromise\":true,\"userGesture\":true}");
    jv res; char *r;
    long long dl = now_ms() + (c->timeout < 3000 ? c->timeout : 3000);
    int rc, tries = 0;
    /* during navigation, or for a page woken from a frozen/background state, the default
     * context can be briefly unknown: enable Runtime once (reports contexts) and retry */
    while ((rc = cdp_call(&c->cdp, sid, "Runtime.evaluate", p.p, c->timeout, &r, &res)) &&
           strstr(slim_errmsg(), "execution context") && now_ms() < dl) {
        if (tries++ == 0 && sid == c->sid && !c->rt_on) {
            if (cdp_call(&c->cdp, sid, "Runtime.enable", NULL, c->timeout, NULL, NULL) == 0) c->rt_on = 1;
            cdp_flush(&c->cdp, sid, "Runtime.consoleAPICalled|Runtime.executionContextCreated");
        } else sleep_ms(50);
    }
    sb_free(&p);
    if (rc) return -1;
    if (exception_err(res)) { free(r); return -1; }
    if (!json_get(res, "result", robj)) { robj->p = "{}"; robj->n = 2; robj->t = J_OBJ; }
    if (resp) *resp = r; else free(r);
    return 0;
}

int ctx_eval(ctx_t *c, const char *expr, char **resp, jv *robj) {
    if (ctx_page(c)) return -1;
    return ctx_eval_in(c, c->sid, expr, resp, robj);
}

int ctx_wait_load(ctx_t *c, const char *events, int ms) {
    char *m;
    int r = cdp_event(&c->cdp, c->sid, events, ms, &m, NULL, NULL);
    if (r < 0) return -1;
    if (r == 0) return slim_seterr("timeout after %d ms waiting for page load", ms);
    free(m);
    return 0;
}

void js_str(sbuf *b, const char *s) { sb_jstrz(b, s); }

static void js_sel(sbuf *b, const char *sel, const char *body, int must) {
    sb_puts(b, "(function(){var s=");
    sb_jstrz(b, sel);
    sb_puts(b, ";var e;if(/^(xpath=|\\/\\/|\\(\\/\\/)/.test(s)){e=document.evaluate(s.indexOf('xpath=')==0?s.slice(6):s,"
               "document,null,9,null).singleNodeValue}else e=document.querySelector(s);");
    if (must) sb_puts(b, "if(!e)throw new Error('no element matches selector '+s);");
    sb_puts(b, body);
    sb_puts(b, "})()");
}
void js_el(sbuf *b, const char *sel, const char *body) { js_sel(b, sel, body, 1); }
void js_find(sbuf *b, const char *sel, const char *body) { js_sel(b, sel, body, 0); }

int ctx_attach(ctx_t *c, const char *tid) {
    if (ctx_browser(c)) return -1;
    char *sid;
    if (cdp_attach(&c->cdp, tid, c->timeout, &sid)) return -1;
    ctx_reset_page(c);
    c->sid = sid;
    c->tid = xstrdup(tid);
    c->tab = NULL;
    ctx_set_target(c, tid);
    return apply_emulation(c);
}

/* Does /proc/pid/cmdline contain the switch arg? Chromium rewrites its cmdline into one
 * space-joined string (setproctitle), so NULs and spaces are both treated as separators. */
int proc_has_arg(int pid, const char *arg, int *is_child) {
    char p[64], *cl; size_t n;
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    if (is_child) *is_child = 0;
    if (read_file(p, &cl, &n, 1 << 20)) return 0;
    for (size_t i = 0; i < n; i++) if (!cl[i]) cl[i] = ' ';
    size_t al = strlen(arg);
    int m = 0;
    for (char *s = cl; (s = strstr(s, arg)); s += al)
        if ((s == cl || s[-1] == ' ') && (s[al] == ' ' || s[al] == 0)) { m = 1; break; }
    if (is_child && strstr(cl, " --type=")) *is_child = 1;
    free(cl);
    return m;
}

/* Find the browser process listening on a DevTools port by scanning /proc cmdlines. */
int find_browser_pid(int port) {
    char want[48];
    snprintf(want, sizeof want, "--remote-debugging-port=%d", port);
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int best = 0;
    while ((e = readdir(d))) {
        long pid; int child;
        if (parse_int(e->d_name, &pid) || pid <= 0) continue;
        if (proc_has_arg((int)pid, want, &child) && !child && (!best || pid < best)) best = (int)pid;
    }
    closedir(d);
    return best;
}
