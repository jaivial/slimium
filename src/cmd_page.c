/* Tabs, navigation, content extraction, screenshots, waits, lifecycle, contexts. */
#include "cmd.h"
#include "hold.h"
#include "b64.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int no_args(ctx_t *c, int argc, char **argv, const char *name) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "%s takes no arguments", name);
    return 0;
}

/* "example.com" -> https://example.com, "./x.html" -> file:///abs/x.html */
static char *norm_url(const char *u) {
    if (strstr(u, "://") || !strncmp(u, "about:", 6) || !strncmp(u, "data:", 5) || !strncmp(u, "javascript:", 11) ||
        !strncmp(u, "chrome:", 7) || !strncmp(u, "blob:", 5))
        return xstrdup(u);
    if (u[0] == '/' || u[0] == '.' || access(u, F_OK) == 0) {
        char rp[PATH_MAX];
        sbuf b = {0};
        sb_puts(&b, "file://");
        sb_puts(&b, realpath(u, rp) ? rp : u);
        return b.p;
    }
    sbuf b = {0};
    sb_puts(&b, !strncmp(u, "localhost", 9) || !strncmp(u, "127.", 4) ? "http://" : "https://");
    sb_puts(&b, u);
    return b.p;
}

static int wait_mode(ctx_t *c, const char *w) {
    if (!w || !strcmp(w, "load")) return 0;
    if (!strcmp(w, "dom")) return 1;
    if (!strcmp(w, "none")) return 2;
    return usage_err(c, "bad --wait '%s' (load|dom|none)", w);
}
static const char *mode_events(int m) { return m == 1 ? "Page.domContentEventFired" : "Page.loadEventFired"; }

static int poll_ready(ctx_t *c, int mode, long long dl) {
    const char *expr = mode == 1 ? "document.readyState!=='loading'" : "document.readyState==='complete'";
    for (;;) {
        char *r; jv o, v;
        if (ctx_eval(c, expr, &r, &o) == 0) {
            int ok = json_get(o, "value", &v) && json_bool(v);
            free(r);
            if (ok) return 0;
        } else if (!strstr(slim_errmsg(), "execution context")) return -1;
        if (now_ms() > dl) return slim_seterr("timeout waiting for page load");
        sleep_ms(50);
    }
}

static int navigate(ctx_t *c, const char *url, int mode) {
    if (ctx_enable(c, "Page")) return -1;
    c->bu_accept = !c->hold_pid;   /* no keeper: accept beforeunload ourselves */
    cdp_flush(&c->cdp, c->sid, "Page.loadEventFired|Page.domContentEventFired");
    sbuf p = {0};
    sb_puts(&p, "{\"url\":"); sb_jstrz(&p, url); sb_putc(&p, '}');
    char *resp; jv res;
    long long t0 = now_ms();
    int r = cdp_call(&c->cdp, c->sid, "Page.navigate", p.p, c->timeout, &resp, &res);
    sb_free(&p);
    if (r) return -1;
    char *et = json_get_str(res, "errorText");
    int same_doc = !json_get(res, "loaderId", NULL);
    free(resp);
    if (et && *et) { slim_seterr("navigation to %s failed: %s", url, et); free(et); return -1; }
    free(et);
    if (same_doc || mode == 2) return 0;
    int left = c->timeout - (int)(now_ms() - t0);
    return ctx_wait_load(c, mode_events(mode), left > 0 ? left : 1);
}

static void out_page(ctx_t *c) {
    char *r; jv o;
    if (ctx_eval(c, "[location.href,document.title]", &r, &o)) return;
    char *u = json_get_str(o, "value.0"), *t = json_get_str(o, "value.1");
    free(r);
    out_s(c, "url", u); out_s(c, "title", t);
    out_t(c, "%s", u ? u : "");
    if (t && *t) out_t(c, "\t%s", t);
    free(u); free(t);
}

int cmd_tabs(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "tabs")) return -1;
    char *resp; jv arr, t; size_t pos = 0;
    if (ctx_state(c) || ctx_targets(c, &resp, &arr)) return -1;
    const char *cur = c->tid ? c->tid : c->have_st ? c->st.target : NULL;
    sb_puts(&c->oj, ",\"tabs\":[");
    int n = 0;
    while (json_next(arr, &pos, NULL, &t)) {
        if (!json_streq_path(t, "type", "page")) continue;
        char *id = json_get_str(t, "targetId"), *u = json_get_str(t, "url"), *ti = json_get_str(t, "title");
        char *bc = json_get_str(t, "browserContextId");
        int is = cur && id && !strcmp(cur, id);
        if (n++) sb_putc(&c->oj, ',');
        sb_puts(&c->oj, "{\"id\":"); sb_jstrz(&c->oj, id);
        sb_puts(&c->oj, ",\"url\":"); sb_jstrz(&c->oj, u);
        sb_puts(&c->oj, ",\"title\":"); sb_jstrz(&c->oj, ti);
        sb_puts(&c->oj, ",\"context\":"); sb_jstrz(&c->oj, bc);
        sb_printf(&c->oj, ",\"current\":%s}", is ? "true" : "false");
        out_t(c, "%s%c %s\t%s\t%s", n > 1 ? "\n" : "", is ? '*' : ' ', id ? id : "", u ? u : "", ti ? ti : "");
        free(id); free(u); free(ti); free(bc);
    }
    sb_putc(&c->oj, ']');
    free(resp);
    return 0;
}

int cmd_open(ctx_t *c, int argc, char **argv) {
    const char *ctxid = opt_val(c, &argc, argv, "--context");
    const char *w = opt_val(c, &argc, argv, "--wait");
    int bg = opt_flag(&argc, argv, "--background");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "open [URL]");
    int mode = wait_mode(c, w);
    if (mode < 0) return -1;
    if (ctx_browser(c)) return -1;
    sbuf p = {0};
    sb_puts(&p, "{\"url\":\"about:blank\"");
    if (ctxid) { sb_puts(&p, ",\"browserContextId\":"); sb_jstrz(&p, ctxid); }
    if (bg) sb_puts(&p, ",\"background\":true");
    sb_putc(&p, '}');
    char *resp; jv res;
    int r = cdp_call(&c->cdp, NULL, "Target.createTarget", p.p, c->timeout, &resp, &res);
    sb_free(&p);
    if (r) return -1;
    char *tid = json_get_str(res, "targetId");
    free(resp);
    if (!tid) return slim_seterr("Target.createTarget: no targetId");
    r = ctx_attach(c, tid);
    out_s(c, "id", tid);
    if (r) { free(tid); return -1; }
    if (argc == 1) {
        char *u = norm_url(argv[0]);
        r = navigate(c, u, mode);
        free(u);
        if (r) { free(tid); return -1; }
    }
    out_t(c, "%s\t", tid);
    out_page(c);
    free(tid);
    return 0;
}

int cmd_close(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "close [ID]");
    char *tid = NULL;
    if (ctx_state(c)) return -1;
    c->allow_dialog = 1;
    if (argc == 1) { if (ctx_resolve_target(c, argv[0], &tid)) return -1; }
    else { if (ctx_page(c)) return -1; tid = xstrdup(c->tid); }
    /* without a keeper a beforeunload handler would keep the tab open behind a dialog: watch the
     * closing tab from a session with Page enabled and accept it */
    char *wsid = NULL, *osid = c->sid;
    if (!c->hold_pid && (!c->have_st || hold_ready(c) == 1)) {
        if (c->tid && !strcmp(c->tid, tid)) { if (ctx_enable(c, "Page") == 0) wsid = xstrdup(c->sid); }
        else if (cdp_attach(&c->cdp, tid, c->timeout, &wsid) == 0 &&
                 cdp_call(&c->cdp, wsid, "Page.enable", NULL, 3000, NULL, NULL)) { free(wsid); wsid = NULL; }
    }
    sbuf p = {0};
    sb_puts(&p, "{\"targetId\":"); sb_jstrz(&p, tid); sb_putc(&p, '}');
    int r = ctx_call(c, 0, "Target.closeTarget", p.p, NULL, NULL);
    sb_free(&p);
    if (!r && wsid) {
        char *m;
        c->sid = wsid; c->bu_accept = 1;
        int got = cdp_event(&c->cdp, NULL, "Target.detachedFromTarget", 3000, &m, NULL, NULL);
        if (got == 1) free(m);
        c->sid = osid; c->bu_accept = 0;
        if (got != 1) r = slim_seterr("tab %s did not close (beforeunload?)", tid);
    }
    free(wsid);
    if (r) { free(tid); return -1; }
    if (c->tid && !strcmp(c->tid, tid)) {
        free(c->sid); c->sid = NULL; free(c->tid); c->tid = NULL;
        c->page_on = c->net_on = c->rt_on = 0;
        cdp_flush(&c->cdp, "*", NULL);
    }
    if (c->have_st && c->st.target && !strcmp(c->st.target, tid)) { state_set(&c->st.target, NULL); state_save(&c->st); }
    out_t(c, "closed %s", tid);
    out_s(c, "id", tid);
    free(tid);
    return 0;
}

int cmd_switch(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "switch ID");
    char *tid;
    if (ctx_state(c) || ctx_resolve_target(c, argv[0], &tid)) return -1;
    sbuf p = {0};
    sb_puts(&p, "{\"targetId\":"); sb_jstrz(&p, tid); sb_putc(&p, '}');
    int r = ctx_call(c, 0, "Target.activateTarget", p.p, NULL, NULL);
    sb_free(&p);
    if (!r) r = ctx_attach(c, tid);
    if (!r) r = cdp_call(&c->cdp, c->sid, "Page.bringToFront", NULL, c->timeout, NULL, NULL); /* make it visible */
    if (!r) { out_s(c, "id", tid); out_t(c, "%s\t", tid); out_page(c); }
    free(tid);
    return r;
}

int cmd_nav(ctx_t *c, int argc, char **argv) {
    const char *w = opt_val(c, &argc, argv, "--wait");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "nav URL [--wait load|dom|none]");
    int mode = wait_mode(c, w);
    if (mode < 0) return -1;
    char *u = norm_url(argv[0]);
    int r = ctx_page(c) || navigate(c, u, mode);
    free(u);
    if (r) return -1;
    out_page(c);
    return 0;
}

static int history(ctx_t *c, int argc, char **argv, int delta) {
    const char *w = opt_val(c, &argc, argv, "--wait");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "%s takes no arguments", delta < 0 ? "back" : "forward");
    int mode = wait_mode(c, w);
    if (mode < 0 || ctx_enable(c, "Page")) return -1;
    c->bu_accept = !c->hold_pid;
    char *resp; jv res, e;
    if (cdp_call(&c->cdp, c->sid, "Page.getNavigationHistory", NULL, c->timeout, &resp, &res)) return -1;
    int cur = (int)json_get_num(res, "currentIndex", -1);
    char key[64]; snprintf(key, sizeof key, "entries.%d", cur + delta);
    if (cur + delta < 0 || !json_get(res, key, &e)) { free(resp); return slim_seterr("no %s history entry", delta < 0 ? "previous" : "next"); }
    long long id = (long long)json_get_num(e, "id", -1);
    free(resp);
    cdp_flush(&c->cdp, c->sid, "Page.frameNavigated|Page.navigatedWithinDocument");
    char p[64]; snprintf(p, sizeof p, "{\"entryId\":%lld}", id);
    long long dl = now_ms() + c->timeout;
    if (cdp_call(&c->cdp, c->sid, "Page.navigateToHistoryEntry", p, c->timeout, NULL, NULL)) return -1;
    if (mode != 2) {
        for (;;) { /* wait for the main frame to commit, then for readiness */
            char *m; jv pr;
            int left = (int)(dl - now_ms());
            int r = cdp_event(&c->cdp, c->sid, "Page.frameNavigated|Page.navigatedWithinDocument", left > 0 ? left : 1, &m, &pr, NULL);
            if (r <= 0) return r ? -1 : slim_seterr("timeout waiting for history navigation");
            int sub = json_get(pr, "frame.parentId", NULL);
            free(m);
            if (!sub) break;
        }
        if (poll_ready(c, mode, dl)) return -1;
    }
    out_page(c);
    return 0;
}
int cmd_back(ctx_t *c, int argc, char **argv) { return history(c, argc, argv, -1); }
int cmd_forward(ctx_t *c, int argc, char **argv) { return history(c, argc, argv, 1); }

int cmd_reload(ctx_t *c, int argc, char **argv) {
    int hard = opt_flag(&argc, argv, "--hard");
    const char *w = opt_val(c, &argc, argv, "--wait");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "reload [--hard] [--wait load|dom|none]");
    int mode = wait_mode(c, w);
    if (mode < 0 || ctx_enable(c, "Page")) return -1;
    c->bu_accept = !c->hold_pid;
    cdp_flush(&c->cdp, c->sid, "Page.loadEventFired|Page.domContentEventFired");
    long long t0 = now_ms();
    if (cdp_call(&c->cdp, c->sid, "Page.reload", hard ? "{\"ignoreCache\":true}" : NULL, c->timeout, NULL, NULL)) return -1;
    if (mode != 2) {
        int left = c->timeout - (int)(now_ms() - t0);
        if (ctx_wait_load(c, mode_events(mode), left > 0 ? left : 1)) return -1;
    }
    out_page(c);
    return 0;
}

/* print a Runtime.RemoteObject (returnByValue) */
static void out_value(ctx_t *c, jv o) {
    jv v, t;
    char *type = json_get(o, "type", &t) ? json_str(t) : xstrdup("undefined");
    if (json_get(o, "value", &v)) {
        out_raw(c, "value", v.p, v.n);
        sb_jv_text(&c->ot, v);
    } else {
        char *u = json_get_str(o, "unserializableValue");
        if (!u) u = json_get_str(o, "description");
        if (u) { out_s(c, "value", u); out_t(c, "%s", u); }
        else out_raw(c, "value", "null", 4);
        free(u);
    }
    out_s(c, "type", type);
    free(type);
}

static int eval_out(ctx_t *c, const char *expr) {
    char *r; jv o;
    if (ctx_eval(c, expr, &r, &o)) return -1;
    out_value(c, o);
    free(r);
    return 0;
}

int cmd_eval(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc < 1) return usage_err(c, "eval JS|-");
    sbuf js = {0};
    if (argc == 1 && !strcmp(argv[0], "-")) {
        if (c->in_batch) return usage_err(c, "eval - is not available in batch");
        char *d; size_t n;
        if (read_fd_all(0, &d, &n, 64u << 20)) return -1;
        sb_add(&js, d, n); free(d);
    } else for (int i = 0; i < argc; i++) { if (i) sb_putc(&js, ' '); sb_puts(&js, argv[i]); }
    int r = eval_out(c, sb_cstr(&js));
    sb_free(&js);
    return r;
}

int cmd_url(ctx_t *c, int argc, char **argv) { return no_args(c, argc, argv, "url") ? -1 : eval_out(c, "location.href"); }
int cmd_title(ctx_t *c, int argc, char **argv) { return no_args(c, argc, argv, "title") ? -1 : eval_out(c, "document.title"); }

int cmd_text(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "text [SELECTOR]");
    if (!argc) return eval_out(c, "(document.body||document.documentElement).innerText");
    sbuf b = {0};
    js_el(&b, argv[0], "return e.innerText!==undefined?e.innerText:e.textContent");
    int r = eval_out(c, b.p);
    sb_free(&b);
    return r;
}

int cmd_html(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "html [SELECTOR]");
    if (!argc) return eval_out(c, "(document.doctype?new XMLSerializer().serializeToString(document.doctype)+'\\n':'')+document.documentElement.outerHTML");
    sbuf b = {0};
    js_el(&b, argv[0], "return e.outerHTML");
    int r = eval_out(c, b.p);
    sb_free(&b);
    return r;
}

/* decode result.data (base64) in place and write it to file */
static int save_b64(ctx_t *c, char *resp, jv res, const char *file) {
    jv d;
    if (!json_get(res, "data", &d) || d.t != J_STR) return slim_seterr("no data in response");
    char *s = (char *)d.p + 1;
    size_t n = d.n - 2;
    if (memchr(s, '\\', n)) { long l = json_unescape(s, n, s); if (l < 0) return slim_seterr("bad data"); n = (size_t)l; }
    long len = b64_decode(s, n, (unsigned char *)s);
    if (len < 0) return slim_seterr("bad base64 data");
    if (!strcmp(file, "-")) {
        if (fwrite(s, 1, (size_t)len, stdout) != (size_t)len) return slim_seterr("write stdout failed");
    } else if (write_file(file, s, (size_t)len, 0)) return -1;
    (void)resp;
    out_s(c, "file", file); out_i(c, "bytes", len);
    if (strcmp(file, "-")) out_t(c, "%s\t%ld bytes", file, len);
    return 0;
}

int cmd_shot(ctx_t *c, int argc, char **argv) {
    int full = opt_flag(&argc, argv, "--full");
    const char *sel = opt_val(c, &argc, argv, "--selector");
    const char *fmt = opt_val(c, &argc, argv, "--format");
    const char *q = opt_val(c, &argc, argv, "--quality");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "shot FILE [--full] [--selector S] [--format png|jpeg|webp] [--quality N]");
    const char *file = argv[0];
    if (!fmt) {
        const char *dot = strrchr(file, '.');
        fmt = dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) ? "jpeg" : dot && !strcasecmp(dot, ".webp") ? "webp" : "png";
    }
    if (!strcmp(fmt, "jpg")) fmt = "jpeg";
    if (strcmp(fmt, "png") && strcmp(fmt, "jpeg") && strcmp(fmt, "webp")) return usage_err(c, "bad --format '%s'", fmt);
    long qv = -1;
    if (q && (parse_int(q, &qv) || qv < 0 || qv > 100)) return usage_err(c, "bad --quality '%s' (0-100)", q);
    if (ctx_page(c)) return -1;
    sbuf p = {0};
    sb_printf(&p, "{\"format\":\"%s\"", fmt);
    if (qv >= 0 && strcmp(fmt, "png")) sb_printf(&p, ",\"quality\":%ld", qv);
    if (sel) {
        sbuf b = {0};
        js_el(&b, sel, "if(e.scrollIntoViewIfNeeded)e.scrollIntoViewIfNeeded(true);else e.scrollIntoView({block:'center'});"
                       "var r=e.getBoundingClientRect();if(!(r.width>0&&r.height>0))throw new Error('element not visible: '+s);"
                       "return [r.left+scrollX,r.top+scrollY,r.width,r.height]");
        char *r; jv o, v;
        int rc = ctx_eval(c, b.p, &r, &o);
        sb_free(&b);
        if (rc) { sb_free(&p); return -1; }
        json_get(o, "value", &v);
        sb_printf(&p, ",\"clip\":{\"x\":%.2f,\"y\":%.2f,\"width\":%.2f,\"height\":%.2f,\"scale\":1},\"captureBeyondViewport\":true",
                  json_get_num(v, "0", 0), json_get_num(v, "1", 0), json_get_num(v, "2", 1), json_get_num(v, "3", 1));
        free(r);
    } else if (full) {
        char *r; jv m;
        if (cdp_call(&c->cdp, c->sid, "Page.getLayoutMetrics", NULL, c->timeout, &r, &m)) { sb_free(&p); return -1; }
        double w = json_get_num(m, "cssContentSize.width", json_get_num(m, "contentSize.width", 800));
        double h = json_get_num(m, "cssContentSize.height", json_get_num(m, "contentSize.height", 600));
        free(r);
        sb_printf(&p, ",\"clip\":{\"x\":0,\"y\":0,\"width\":%.0f,\"height\":%.0f,\"scale\":1},\"captureBeyondViewport\":true", w, h);
    }
    sb_putc(&p, '}');
    char *resp; jv res;
    int r = cdp_call(&c->cdp, c->sid, "Page.captureScreenshot", p.p, c->timeout, &resp, &res);
    sb_free(&p);
    if (r) return -1;
    r = save_b64(c, resp, res, file);
    free(resp);
    if (!r) out_s(c, "format", fmt);
    return r;
}

int cmd_pdf(ctx_t *c, int argc, char **argv) {
    int land = opt_flag(&argc, argv, "--landscape");
    int bgf = opt_flag(&argc, argv, "--background");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "pdf FILE [--landscape] [--background]");
    char p[96];
    snprintf(p, sizeof p, "{\"landscape\":%s,\"printBackground\":%s}", land ? "true" : "false", bgf ? "true" : "false");
    char *resp; jv res;
    if (ctx_call(c, 1, "Page.printToPDF", p, &resp, &res)) return -1;
    int r = save_b64(c, resp, res, argv[0]);
    free(resp);
    return r;
}

static void frame_walk(ctx_t *c, jv node, const char *parent, int depth, int *n) {
    jv f, kids, k;
    if (!json_get(node, "frame", &f)) return;
    char *id = json_get_str(f, "id"), *u = json_get_str(f, "url"), *nm = json_get_str(f, "name");
    if ((*n)++) sb_putc(&c->oj, ',');
    sb_puts(&c->oj, "{\"id\":"); sb_jstrz(&c->oj, id);
    sb_puts(&c->oj, ",\"parent\":"); sb_jstrz(&c->oj, parent);
    sb_puts(&c->oj, ",\"url\":"); sb_jstrz(&c->oj, u);
    sb_puts(&c->oj, ",\"name\":"); sb_jstrz(&c->oj, nm);
    sb_printf(&c->oj, ",\"depth\":%d}", depth);
    out_t(c, "%s%*s%s\t%s%s%s", *n > 1 ? "\n" : "", depth * 2, "", id ? id : "", u ? u : "", nm && *nm ? "\tname=" : "", nm && *nm ? nm : "");
    if (json_get(node, "childFrames", &kids)) {
        size_t pos = 0;
        while (json_next(kids, &pos, NULL, &k)) frame_walk(c, k, id, depth + 1, n);
    }
    free(id); free(u); free(nm);
}

int cmd_frames(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "frames")) return -1;
    char *resp; jv res, t;
    if (ctx_call(c, 1, "Page.getFrameTree", NULL, &resp, &res)) return -1;
    int n = 0;
    sb_puts(&c->oj, ",\"frames\":[");
    if (json_get(res, "frameTree", &t)) frame_walk(c, t, NULL, 0, &n);
    sb_putc(&c->oj, ']');
    free(resp);
    return 0;
}

int cmd_wait(ctx_t *c, int argc, char **argv) {
    int vis = opt_flag(&argc, argv, "--visible");
    int gone = opt_flag(&argc, argv, "--gone");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "wait SELECTOR [--visible] [--gone] [--timeout MS]");
    sbuf b = {0}, body = {0};
    sb_puts(&body, vis ? "var ok=!!e&&(function(){var r=e.getBoundingClientRect(),st=getComputedStyle(e);"
                         "return r.width>0&&r.height>0&&st.visibility!=='hidden'&&st.display!=='none'})();"
                       : "var ok=!!e;");
    sb_puts(&body, gone ? "return !ok" : "return ok");
    js_find(&b, argv[0], body.p);
    sb_free(&body);
    long long t0 = now_ms(), dl = t0 + c->timeout;
    int r = -1;
    for (;;) {
        char *resp; jv o, v;
        if (ctx_eval(c, b.p, &resp, &o)) break;
        int ok = json_get(o, "value", &v) && json_bool(v);
        free(resp);
        if (ok) { r = 0; break; }
        if (now_ms() >= dl) { slim_seterr("timeout after %d ms waiting for %s%s", c->timeout, argv[0], gone ? " to disappear" : ""); break; }
        sleep_ms(50);
    }
    sb_free(&b);
    if (!r) { out_i(c, "ms", now_ms() - t0); out_t(c, "found %s after %lld ms", argv[0], now_ms() - t0); }
    return r;
}

int cmd_wait_load(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "wait-load")) return -1;
    long long t0 = now_ms();
    if (poll_ready(c, 0, t0 + c->timeout)) return -1;
    out_i(c, "ms", now_ms() - t0);
    out_t(c, "loaded after %lld ms", now_ms() - t0);
    return 0;
}

int cmd_wait_idle(ctx_t *c, int argc, char **argv) {
    int idle = opt_ms(c, &argc, argv, "--idle", 500);
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "wait-idle [--idle MS] [--timeout MS]");
    long long t0 = now_ms(), dl = t0 + c->timeout;
    if (ctx_enable(c, "Network")) return -1;
    if (poll_ready(c, 0, dl)) return -1;
    char **ids = NULL; size_t nid = 0, cap = 0;
    long long last = now_ms();
    int r = 0;
    for (;;) {
        long long now = now_ms();
        if (nid == 0 && now - last >= idle) break;
        if (now >= dl) { r = slim_seterr("timeout after %d ms waiting for network idle (%zu requests pending)", c->timeout, nid); break; }
        long long wait = nid ? dl - now : idle - (now - last);
        if (wait > dl - now) wait = dl - now;
        char *m; jv pr, mm;
        int e = cdp_event(&c->cdp, c->sid, "Network.requestWillBeSent|Network.loadingFinished|Network.loadingFailed", (int)(wait > 0 ? wait : 1), &m, &pr, &mm);
        if (e < 0) { r = -1; break; }
        if (e == 0) continue;
        char *rid = json_get_str(pr, "requestId");
        if (rid) {
            size_t i;
            for (i = 0; i < nid && strcmp(ids[i], rid); i++) {}
            if (json_str_eq(mm, "Network.requestWillBeSent")) {
                if (i == nid) {
                    if (nid == cap) { cap = cap ? cap * 2 : 16; ids = xrealloc(ids, cap * sizeof *ids); }
                    ids[nid++] = rid; rid = NULL;
                }
            } else if (i < nid) { free(ids[i]); ids[i] = ids[--nid]; }
            last = now_ms();
        }
        free(rid); free(m);
    }
    for (size_t i = 0; i < nid; i++) free(ids[i]);
    free(ids);
    if (!r) { out_i(c, "ms", now_ms() - t0); out_t(c, "idle after %lld ms", now_ms() - t0); }
    return r;
}

static int lifecycle(ctx_t *c, int argc, char **argv, const char *state) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "%s [ID]", !strcmp(state, "frozen") ? "freeze" : "thaw");
    char p[48]; snprintf(p, sizeof p, "{\"state\":\"%s\"}", state);
    if (ctx_state(c)) return -1;
    if (argc == 0) {
        if (ctx_call(c, 1, "Page.setWebLifecycleState", p, NULL, NULL)) return -1;
        out_s(c, "id", c->tid); out_s(c, "state", state); out_t(c, "%s %s", c->tid, state);
        return 0;
    }
    char *tid, *sid;
    if (ctx_resolve_target(c, argv[0], &tid)) return -1;
    if (cdp_attach(&c->cdp, tid, c->timeout, &sid)) { free(tid); return -1; }
    int r = cdp_call(&c->cdp, sid, "Page.setWebLifecycleState", p, c->timeout, NULL, NULL);
    cdp_detach(&c->cdp, sid, 2000);
    free(sid);
    if (!r) { out_s(c, "id", tid); out_s(c, "state", state); out_t(c, "%s %s", tid, state); }
    free(tid);
    return r;
}
int cmd_freeze(ctx_t *c, int argc, char **argv) { return lifecycle(c, argc, argv, "frozen"); }
int cmd_thaw(ctx_t *c, int argc, char **argv) { return lifecycle(c, argc, argv, "active"); }

int cmd_context_new(ctx_t *c, int argc, char **argv) {
    const char *url = opt_val(c, &argc, argv, "--url");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "context-new [--url URL]");
    char *resp; jv res;
    if (ctx_call(c, 0, "Target.createBrowserContext", "{\"disposeOnDetach\":false}", &resp, &res)) return -1;
    char *id = json_get_str(res, "browserContextId");
    free(resp);
    if (!id) return slim_seterr("no browserContextId");
    out_s(c, "context", id);
    out_t(c, "%s", id);
    if (url) {
        char *argv2[4] = {(char *)url, "--context", id, NULL};
        sbuf saved_t = c->ot; memset(&c->ot, 0, sizeof c->ot);
        int r = cmd_open(c, 3, argv2);
        sb_free(&c->ot); c->ot = saved_t;
        if (r) { free(id); return -1; }
        out_t(c, "\t%s", c->tid);
    }
    free(id);
    return 0;
}

int cmd_context_close(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "context-close ID");
    sbuf p = {0};
    sb_puts(&p, "{\"browserContextId\":"); sb_jstrz(&p, argv[0]); sb_putc(&p, '}');
    int r = ctx_call(c, 0, "Target.disposeBrowserContext", p.p, NULL, NULL);
    sb_free(&p);
    if (r) return -1;
    if (c->sid) { free(c->sid); c->sid = NULL; free(c->tid); c->tid = NULL; c->page_on = c->net_on = c->rt_on = 0; }
    out_s(c, "context", argv[0]); out_t(c, "closed context %s", argv[0]);
    return 0;
}

int cmd_contexts(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "contexts")) return -1;
    char *resp; jv res, a, v;
    if (ctx_call(c, 0, "Target.getBrowserContexts", NULL, &resp, &res)) return -1;
    if (json_get(res, "browserContextIds", &a)) {
        out_raw(c, "contexts", a.p, a.n);
        size_t pos = 0; int n = 0;
        while (json_next(a, &pos, NULL, &v)) { out_t(c, "%s", n++ ? "\n" : ""); sb_jv_text(&c->ot, v); }
    }
    free(resp);
    return 0;
}
