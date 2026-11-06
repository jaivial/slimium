/* Media commands: detached recording and live responsive captures. */
#include "cmd.h"
#include "record.h"
#include "b64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmd_resize_alias(ctx_t *c, int argc, char **argv) {
    return cmd_viewport(c, argc, argv);
}

/* through the session state, so the keeper (owner of the overrides) applies each size */
static int live_viewport(ctx_t *c, int w, int h, int mobile, double scale, int orient, int touch) {
    if (ctx_state_reload(c)) return -1;
    c->st.vp_w = w; c->st.vp_h = h; c->st.vp_mobile = mobile; c->st.vp_scale = scale;
    c->st.vp_orient = orient; c->st.vp_touch = touch;
    return emu_commit(c);
}

static int save_png(ctx_t *c, const char *path) {
    char *resp = NULL; jv res, data;
    if (cdp_call(&c->cdp, c->sid, "Page.captureScreenshot",
                 "{\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":false}",
                 c->timeout, &resp, &res)) return -1;
    if (!json_get(res, "data", &data) || data.t != J_STR) { free(resp); return slim_seterr("screenshot has no data"); }
    char *s = (char *)data.p + 1; size_t n = data.n - 2;
    if (memchr(s, '\\', n)) {
        long u = json_unescape(s, n, s);
        if (u < 0) { free(resp); return slim_seterr("bad screenshot data"); }
        n = (size_t)u;
    }
    long decoded = b64_decode(s, n, (unsigned char *)s);
    int rc = 0;
    if (decoded < 0) rc = slim_seterr("bad screenshot base64");
    else if (write_file(path, s, (size_t)decoded, 0)) rc = -1;
    free(resp);
    return rc;
}

static int cmd_responsive(ctx_t *c, int argc, char **argv) {
    const char *sizes = opt_val(c, &argc, argv, "--sizes");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1 || !sizes || !*sizes) return usage_err(c, "responsive OUTDIR --sizes 375x812,768x1024,1440x900");
    char *copy = xstrdup(sizes), *save = NULL;
    int widths[32], heights[32], count = 0;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (count == 32) { free(copy); return usage_err(c, "responsive supports at most 32 sizes"); }
        char *x = strchr(tok, 'x'); long w, h;
        if (!x) { free(copy); return usage_err(c, "bad viewport size '%s'", tok); }
        *x++ = 0;
        if (parse_int(tok, &w) || parse_int(x, &h) || w < 1 || h < 1 || w > 100000 || h > 100000) {
            free(copy); return usage_err(c, "bad viewport size '%sx%s'", tok, x);
        }
        widths[count] = (int)w; heights[count] = (int)h; count++;
    }
    free(copy);
    if (!count) return usage_err(c, "responsive needs at least one size");
    if (ctx_state(c) || ctx_page(c)) return -1;
    if (mkdir_p(argv[0], 0755)) return -1;
    int old_w = c->have_st ? c->st.vp_w : 0, old_h = c->have_st ? c->st.vp_h : 0;
    int old_mobile = c->have_st ? c->st.vp_mobile : 0;
    int old_orient = c->have_st ? c->st.vp_orient : 0, old_touch = c->have_st ? c->st.vp_touch : 0;
    double old_scale = c->have_st ? c->st.vp_scale : 0;
    int rc = -1, made = 0;
    sbuf json = {0}; sb_putc(&json, '[');
    for (int i = 0; i < count; i++) {
        if (live_viewport(c, widths[i], heights[i], 0, 0, 0, 0)) goto done;
        char *resp = NULL; jv obj, value;
        const char *expr = "new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve([innerWidth,innerHeight]))))";
        if (ctx_eval(c, expr, &resp, &obj)) goto done;
        int have = json_get(obj, "value", &value);
        int actual_w = have ? (int)json_get_num(value, "0", -1) : -1;
        int actual_h = have ? (int)json_get_num(value, "1", -1) : -1;
        free(resp);
        if (actual_w != widths[i] || actual_h != heights[i]) {
            slim_seterr("viewport %dx%d did not apply (got %dx%d)", widths[i], heights[i], actual_w, actual_h);
            goto done;
        }
        char path[4096];
        int k = snprintf(path, sizeof path, "%s/%dx%d.png", argv[0], widths[i], heights[i]);
        if (k < 0 || (size_t)k >= sizeof path) { slim_seterr("responsive output path too long"); goto done; }
        if (save_png(c, path)) goto done;
        struct stat st;
        if (stat(path, &st)) { slim_seterr("stat %s: %s", path, strerror(errno)); goto done; }
        if (made++) sb_putc(&json, ',');
        sb_puts(&json, "{\"file\":"); sb_jstrz(&json, path);
        sb_printf(&json, ",\"width\":%d,\"height\":%d,\"bytes\":%lld}", widths[i], heights[i], (long long)st.st_size);
        out_t(c, "%s%s\t%dx%d\t%lld bytes", made > 1 ? "\n" : "", path, widths[i], heights[i], (long long)st.st_size);
    }
    sb_putc(&json, ']');
    rc = 0;
done:
    if (live_viewport(c, old_w, old_h, old_mobile, old_scale, old_orient, old_touch)) rc = -1;
    if (!rc) {
        out_raw(c, "captures", json.p, json.n); out_i(c, "count", made);
        out_i(c, "restored_width", old_w); out_i(c, "restored_height", old_h);
    }
    sb_free(&json);
    return rc;
}

static const cmd_def media_commands[] = {
    {"record", record_command, 0, "start FILE [--fps N] [--format mp4|webm|avi] [--max-seconds N] [--quality N] | stop [--id ID] | status | list"},
    {"responsive", cmd_responsive, 0, "OUTDIR --sizes WxH,...  capture live page at each viewport, then restore"},
    {"resize", cmd_resize_alias, 0, "W H [--scale F] [--mobile] [--touch|--no-touch] [--orientation O] | mobile|tablet|desktop | reset  alias for viewport"},
    {"__record-worker", record_worker_command, CF_NOBATCH, "internal detached recording worker"},
};

const cmd_def *slim_media_commands(size_t *count) {
    *count = sizeof media_commands / sizeof media_commands[0];
    return media_commands;
}
