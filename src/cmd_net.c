/* Cookies, storage, emulation (persisted per session), downloads, console. */
#include "cmd.h"
#include "hold.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int cmd_cookies(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "cookies takes no arguments");
    char *resp; jv res, arr, k;
    if (ctx_call(c, 0, "Storage.getCookies", NULL, &resp, &res)) return -1;
    if (!json_get(res, "cookies", &arr)) { free(resp); return slim_seterr("bad Storage.getCookies response"); }
    out_raw(c, "cookies", arr.p, arr.n);
    size_t pos = 0; int n = 0;
    while (json_next(arr, &pos, NULL, &k)) {
        char *nm = json_get_str(k, "name"), *v = json_get_str(k, "value"), *d = json_get_str(k, "domain"), *p = json_get_str(k, "path");
        out_t(c, "%s%s=%s\t%s\t%s", n++ ? "\n" : "", nm ? nm : "", v ? v : "", d ? d : "", p ? p : "");
        free(nm); free(v); free(d); free(p);
    }
    free(resp);
    return 0;
}

int cmd_cookie_set(ctx_t *c, int argc, char **argv) {
    const char *dom = opt_val(c, &argc, argv, "--domain");
    const char *path = opt_val(c, &argc, argv, "--path");
    const char *url = opt_val(c, &argc, argv, "--url");
    int secure = opt_flag(&argc, argv, "--secure");
    int httponly = opt_flag(&argc, argv, "--httponly");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "cookie-set NAME VALUE [--domain D] [--path P] [--url U]");
    char *cur = NULL;
    if (!dom && !url) {
        char *r; jv o;
        if (ctx_eval(c, "location.href", &r, &o)) return -1;
        cur = json_get_str(o, "value");
        free(r);
        if (!cur || strncmp(cur, "http", 4)) { free(cur); return slim_seterr("current page is not http(s); pass --domain or --url"); }
        url = cur;
    }
    sbuf p = {0};
    sb_puts(&p, "{\"cookies\":[{\"name\":"); sb_jstrz(&p, argv[0]);
    sb_puts(&p, ",\"value\":"); sb_jstrz(&p, argv[1]);
    if (dom) { sb_puts(&p, ",\"domain\":"); sb_jstrz(&p, dom); }
    if (url) { sb_puts(&p, ",\"url\":"); sb_jstrz(&p, url); }
    sb_puts(&p, ",\"path\":"); sb_jstrz(&p, path ? path : "/");
    if (secure) sb_puts(&p, ",\"secure\":true");
    if (httponly) sb_puts(&p, ",\"httpOnly\":true");
    sb_puts(&p, "}]}");
    int r = ctx_call(c, 0, "Storage.setCookies", p.p, NULL, NULL);
    sb_free(&p);
    free(cur);
    if (r) return -1;
    out_s(c, "name", argv[0]);
    out_t(c, "cookie %s set", argv[0]);
    return 0;
}

int cmd_cookies_clear(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "cookies-clear takes no arguments");
    if (ctx_call(c, 0, "Storage.clearCookies", NULL, NULL, NULL)) return -1;
    out_t(c, "cookies cleared");
    return 0;
}

int cmd_storage_clear(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "storage-clear takes no arguments");
    char *r; jv o;
    if (ctx_eval(c, "(function(){try{localStorage.clear()}catch(e){}try{sessionStorage.clear()}catch(e){}return location.origin})()", &r, &o)) return -1;
    char *origin = json_get_str(o, "value");
    free(r);
    if (origin && strcmp(origin, "null") && strncmp(origin, "file:", 5)) {
        sbuf p = {0};
        sb_puts(&p, "{\"origin\":"); sb_jstrz(&p, origin); sb_puts(&p, ",\"storageTypes\":\"all\"}");
        int rc = ctx_call(c, 0, "Storage.clearDataForOrigin", p.p, NULL, NULL);
        sb_free(&p);
        if (rc) { free(origin); return -1; }
    }
    out_s(c, "origin", origin);
    out_t(c, "storage cleared for %s", origin ? origin : "?");
    free(origin);
    return 0;
}

int cmd_viewport(ctx_t *c, int argc, char **argv) {
    const char *sc = opt_val(c, &argc, argv, "--scale");
    const char *ori = opt_val(c, &argc, argv, "--orientation");
    int mobile = opt_flag(&argc, argv, "--mobile");
    int touch = opt_flag(&argc, argv, "--touch"), notouch = opt_flag(&argc, argv, "--no-touch");
    if (opt_done(c, &argc, argv)) return -1;
    if (ctx_state_reload(c)) return -1;
    slim_state *s = &c->st;
    if (argc == 1 && !strcmp(argv[0], "reset")) {
        if (sc || ori || mobile || touch || notouch) return usage_err(c, "viewport reset takes no options");
        s->vp_w = s->vp_h = 0; s->vp_scale = 0; s->vp_mobile = s->vp_orient = s->vp_touch = 0;
        state_set(&s->ua_preset, NULL);
        if (emu_commit(c)) return -1;
        out_t(c, "viewport reset");
        return 0;
    }
    long w = 0, h = 0; double scale = 0;
    const char *preset = NULL;
    if (argc == 1 && (!strcmp(argv[0], "mobile") || !strcmp(argv[0], "tablet") || !strcmp(argv[0], "desktop"))) {
        preset = argv[0];
        if (!strcmp(preset, "mobile")) { w = 390; h = 844; scale = 3; mobile = 1; }
        else if (!strcmp(preset, "tablet")) { w = 768; h = 1024; scale = 2; mobile = 1; }
        else { w = 1440; h = 900; scale = 1; }
    } else if (argc != 2 || parse_int(argv[0], &w) || parse_int(argv[1], &h) || w < 1 || h < 1 || w > 10000000 || h > 10000000) {
        return usage_err(c, "viewport W H [--scale F] [--mobile] [--touch|--no-touch] [--orientation portrait|landscape|auto] | viewport mobile|tablet|desktop | viewport reset");
    }
    if (sc && (parse_double(sc, &scale) || scale <= 0 || scale > 10)) return usage_err(c, "bad --scale '%s'", sc);
    if (touch && notouch) return usage_err(c, "use only one of --touch, --no-touch");
    int o = 0;
    if (ori && !strcmp(ori, "portrait")) o = 1;
    else if (ori && !strcmp(ori, "landscape")) o = 2;
    else if (ori && strcmp(ori, "auto")) return usage_err(c, "bad --orientation '%s' (portrait|landscape|auto)", ori);
    s->vp_w = (int)w; s->vp_h = (int)h; s->vp_scale = scale; s->vp_mobile = mobile; s->vp_orient = o;
    s->vp_touch = touch ? 1 : notouch ? 2 : 0;
    state_set(&s->ua_preset, preset && strcmp(preset, "desktop") ? preset : NULL);
    if (emu_commit(c)) return -1;
    int t = emu_touch(s), por = emu_portrait(s);
    out_i(c, "width", w); out_i(c, "height", h); out_d(c, "scale", scale); out_b(c, "mobile", mobile);
    out_b(c, "touch", t); out_s(c, "orientation", por ? "portrait" : "landscape");
    if (preset) out_s(c, "preset", preset);
    out_b(c, "keeper", c->hold_pid > 0);
    out_t(c, "viewport %ldx%ld%s%s %s", w, h, mobile ? " mobile" : "", t ? " touch" : "", por ? "portrait" : "landscape");
    if (scale > 0) out_t(c, " dpr %g", scale);
    return 0;
}

int cmd_ua(ctx_t *c, int argc, char **argv) {
    int reset = opt_flag(&argc, argv, "--reset");
    if (opt_done(c, &argc, argv)) return -1;
    if (reset ? argc != 0 : argc != 1) return usage_err(c, "ua STRING | ua --reset");
    if (ctx_state_reload(c)) return -1;
    state_set(&c->st.ua, reset ? NULL : argv[0]);
    if (emu_commit(c)) return -1;
    out_s(c, "ua", reset ? NULL : argv[0]);
    out_t(c, reset ? "user agent reset" : "user agent set");
    return 0;
}

int cmd_offline(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off"))) return usage_err(c, "offline on|off");
    int on = !strcmp(argv[0], "on");
    if (ctx_state_reload(c)) return -1;
    c->st.offline = on;
    if (emu_commit(c)) return -1;
    out_b(c, "offline", on);
    out_t(c, "offline %s", argv[0]);
    return 0;
}

int cmd_block(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (ctx_state_reload(c)) return -1;
    sbuf a = {0};
    sb_putc(&a, '[');
    for (int i = 0; i < argc; i++) { if (i) sb_putc(&a, ','); sb_jstrz(&a, argv[i]); }
    sb_putc(&a, ']');
    state_set(&c->st.block, argc ? a.p : NULL);
    if (emu_commit(c)) { sb_free(&a); return -1; }
    out_raw(c, "blocked", a.p, a.n);
    if (argc) out_t(c, "blocking %d pattern%s", argc, argc > 1 ? "s" : ""); else out_t(c, "unblocked");
    sb_free(&a);
    return 0;
}

int cmd_download_dir(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "download-dir DIR | off");
    if (ctx_state(c)) return -1;
    int off = !strcmp(argv[0], "off");
    char rp[PATH_MAX];
    if (!off) {
        if (mkdir_p(argv[0], 0755)) return -1;
        if (!realpath(argv[0], rp)) return slim_seterr("bad directory %s", argv[0]);
    }
    sbuf p = {0};
    if (off) sb_puts(&p, "{\"behavior\":\"default\"}");
    else { sb_puts(&p, "{\"behavior\":\"allow\",\"downloadPath\":"); sb_jstrz(&p, rp); sb_puts(&p, ",\"eventsEnabled\":false}"); }
    int r = ctx_call(c, 0, "Browser.setDownloadBehavior", p.p, NULL, NULL);
    sb_free(&p);
    if (r) return -1;
    if (c->have_st) { state_set(&c->st.dldir, off ? NULL : rp); state_save(&c->st); }
    out_s(c, "dir", off ? NULL : rp);
    out_t(c, off ? "downloads: default behavior" : "downloads go to %s", rp);
    return 0;
}

static void remote_text(sbuf *b, jv o) {
    jv v;
    if (json_get(o, "value", &v)) { sb_jv_text(b, v); return; }
    char *d = json_get_str(o, "unserializableValue");
    if (!d) d = json_get_str(o, "description");
    if (!d) d = json_get_str(o, "type");
    if (d) sb_puts(b, d);
    free(d);
}

int cmd_console(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "console [--timeout MS]");
    int dur = c->timeout_set ? c->timeout : 1000;
    if (ctx_page(c)) return -1;
    cdp_flush(&c->cdp, c->sid, "Runtime.consoleAPICalled|Runtime.exceptionThrown|Log.entryAdded");
    long id1, id2;
    if (cdp_send(&c->cdp, c->sid, "Runtime.enable", NULL, &id1) || cdp_send(&c->cdp, c->sid, "Log.enable", NULL, &id2)) return -1;
    if (cdp_wait(&c->cdp, id1, c->sid, 10000, NULL, NULL) || cdp_wait(&c->cdp, id2, c->sid, 10000, NULL, NULL)) return -1;
    c->rt_on = 1;
    long long dl = now_ms() + dur;
    sb_puts(&c->oj, ",\"messages\":[");
    int n = 0;
    sbuf t = {0};
    for (;;) {
        long long left = dl - now_ms();
        if (left <= 0) break;
        char *m; jv pr, mm;
        int r = cdp_event(&c->cdp, c->sid, "Runtime.consoleAPICalled|Runtime.exceptionThrown|Log.entryAdded", (int)left, &m, &pr, &mm);
        if (r < 0) { sb_free(&t); return -1; }
        if (r == 0) break;
        sb_reset(&t);
        char *type = NULL;
        if (json_str_eq(mm, "Runtime.consoleAPICalled")) {
            type = json_get_str(pr, "type");
            jv args, a; size_t pos = 0; int k = 0;
            if (json_get(pr, "args", &args))
                while (json_next(args, &pos, NULL, &a)) { if (k++) sb_putc(&t, ' '); remote_text(&t, a); }
        } else if (json_str_eq(mm, "Runtime.exceptionThrown")) {
            type = xstrdup("exception");
            char *d = json_get_str(pr, "exceptionDetails.exception.description");
            if (!d) d = json_get_str(pr, "exceptionDetails.text");
            if (d) sb_puts(&t, d);
            free(d);
        } else {
            type = json_get_str(pr, "entry.level");
            char *src = json_get_str(pr, "entry.source"), *tx = json_get_str(pr, "entry.text");
            sb_printf(&t, "[%s] %s", src ? src : "log", tx ? tx : "");
            free(src); free(tx);
        }
        if (n++) sb_putc(&c->oj, ',');
        sb_puts(&c->oj, "{\"type\":"); sb_jstrz(&c->oj, type);
        sb_puts(&c->oj, ",\"text\":"); sb_jstr(&c->oj, sb_cstr(&t), t.n);
        sb_putc(&c->oj, '}');
        out_t(c, "%s%s\t", n > 1 ? "\n" : "", type ? type : "log");
        sb_add(&c->ot, sb_cstr(&t), t.n);
        free(type); free(m);
    }
    sb_putc(&c->oj, ']');
    sb_free(&t);
    return 0;
}