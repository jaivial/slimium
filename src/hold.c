/* Emulation keeper (internal command '__hold') and its client helpers. */
#include "hold.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACK_MS 3000      /* a page that does not answer (e.g. renderer hung) does not block the ack */
#define WAIT_MS 5000     /* commands wait at most this long for the keeper */
#define LOG_MAX 65536    /* dialog log is trimmed to half when it grows beyond this */

/* ---------------- emulation plan ---------------- */
int emu_portrait(const slim_state *s) { return s->vp_orient ? s->vp_orient == 1 : s->vp_h >= s->vp_w; }
int emu_touch(const slim_state *s) { return s->vp_w > 0 && (s->vp_touch ? s->vp_touch == 1 : s->vp_mobile); }

static void step(emu_step *o, int *n, const char *m, const char *p) { o[*n].m = m; o[*n].p = xstrdup(p ? p : "{}"); (*n)++; }

/* Chromium's GREASE brand for a major version (user_agent_utils.cc) */
static void brands(sbuf *b, int major, const char *full, int list) {
    static const char ch[] = " (:-./);=?_";
    static const char *const gv[] = {"8", "99", "24"};
    char g[24], gvv[16], cv[40];
    snprintf(g, sizeof g, "Not%cA%cBrand", ch[major % 11], ch[(major + 1) % 11]);
    snprintf(gvv, sizeof gvv, list ? "%s.0.0.0" : "%s", gv[major % 3]);
    if (list && full) snprintf(cv, sizeof cv, "%s", full); else snprintf(cv, sizeof cv, list ? "%d.0.0.0" : "%d", major);
    char a[80], z[80];
    snprintf(a, sizeof a, "{\"brand\":\"%s\",\"version\":\"%s\"}", g, gvv);
    snprintf(z, sizeof z, "{\"brand\":\"Chromium\",\"version\":\"%s\"}", cv);
    sb_printf(b, "[%s,%s]", major % 2 ? z : a, major % 2 ? a : z);
}

/* setUserAgentOverride params, or 0 when the browser default stays */
static int ua_params(const slim_state *s, const char *base, const char *ver, sbuf *p) {
    if (s->ua) { sb_puts(p, "{\"userAgent\":"); sb_jstrz(p, s->ua); sb_putc(p, '}'); return 1; }
    int major = 0;
    const char *k = base ? strstr(base, "Chrome/") : NULL;
    if (k) major = atoi(k + 7);
    if (!major && ver) major = atoi(ver);
    if (major <= 0) return 0;
    const char *fv = ver && atoi(ver) == major ? ver : NULL;
    sbuf ua = {0};
    const char *plat, *pv = "", *arch = "", *bits = "", *model = "", *ff, *navp = NULL;
    int mobile = 0;
    if (s->ua_preset && (!strcmp(s->ua_preset, "mobile") || !strcmp(s->ua_preset, "tablet"))) {
        mobile = !strcmp(s->ua_preset, "mobile");
        sb_printf(&ua, "Mozilla/5.0 (Linux; Android 14%s) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/%d.0.0.0 %sSafari/537.36",
                  mobile ? "; Pixel 8" : "", major, mobile ? "Mobile " : "");
        plat = "Android"; pv = "14.0.0"; model = mobile ? "Pixel 8" : ""; ff = mobile ? "Mobile" : "Tablet"; navp = "Linux armv8l";
    } else {
        const char *e = getenv("SLIM_KEEP_HEADLESS_UA");
        const char *h = base ? strstr(base, "HeadlessChrome/") : NULL;
        if (!h || (e && *e && strcmp(e, "0"))) return 0;
        sb_add(&ua, base, (size_t)(h - base)); sb_puts(&ua, h + 8);
        struct utsname u;
        int arm = uname(&u) == 0 && !strncmp(u.machine, "a", 1);
        plat = "Linux"; arch = arm ? "arm" : "x86"; bits = "64"; ff = "Desktop";
    }
    sb_puts(p, "{\"userAgent\":"); sb_jstrz(p, ua.p);
    if (navp) { sb_puts(p, ",\"platform\":"); sb_jstrz(p, navp); }
    sb_puts(p, ",\"userAgentMetadata\":{\"brands\":"); brands(p, major, fv, 0);
    sb_puts(p, ",\"fullVersionList\":"); brands(p, major, fv, 1);
    if (fv) sb_printf(p, ",\"fullVersion\":\"%s\"", fv); else sb_printf(p, ",\"fullVersion\":\"%d.0.0.0\"", major);
    sb_printf(p, ",\"platform\":\"%s\",\"platformVersion\":\"%s\",\"architecture\":\"%s\",\"bitness\":\"%s\",\"model\":\"%s\",\"mobile\":%s,\"wow64\":false,\"formFactors\":[\"%s\"]}}",
              plat, pv, arch, bits, model, mobile ? "true" : "false", ff);
    sb_free(&ua);
    return 1;
}

int emu_plan(const slim_state *s, const char *base_ua, const char *version, int full, int clear, int net_on, emu_step *o) {
    int n = 0;
    sbuf p = {0};
    if (clear) step(o, &n, "Emulation.clearDeviceMetricsOverride", NULL);
    if (s->vp_w > 0) {
        int por = emu_portrait(s);
        sb_printf(&p, "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%g,\"mobile\":%s,\"screenWidth\":%d,\"screenHeight\":%d,"
                      "\"screenOrientation\":{\"type\":\"%s\",\"angle\":%d}}",
                  s->vp_w, s->vp_h, s->vp_scale, s->vp_mobile ? "true" : "false", s->vp_w, s->vp_h,
                  por ? "portraitPrimary" : "landscapePrimary", por ? 0 : 90);
        step(o, &n, "Emulation.setDeviceMetricsOverride", p.p);
    } else if (full && !clear) step(o, &n, "Emulation.clearDeviceMetricsOverride", NULL);
    int t = emu_touch(s);
    /* not setEmitTouchEventsForMouse: it swallows Input.dispatchMouseEvent (mousePressed never
     * answers); clicks become real touch taps instead (ctx_tap) */
    if (t || full) step(o, &n, "Emulation.setTouchEmulationEnabled", t ? "{\"enabled\":true,\"maxTouchPoints\":5}" : "{\"enabled\":false}");
    sb_reset(&p);
    if (ua_params(s, base_ua, version, &p)) step(o, &n, "Emulation.setUserAgentOverride", p.p);
    else if (full) step(o, &n, "Emulation.setUserAgentOverride", "{\"userAgent\":\"\"}");
    if (s->offline || s->block) {
        step(o, &n, "Network.enable", NULL);
        step(o, &n, "Network.emulateNetworkConditions", s->offline ? "{\"offline\":true,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}"
                                                                   : "{\"offline\":false,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}");
        sb_reset(&p); sb_puts(&p, "{\"urls\":"); sb_puts(&p, s->block ? s->block : "[]"); sb_putc(&p, '}');
        step(o, &n, "Network.setBlockedURLs", p.p);
    } else if (full && net_on) {
        step(o, &n, "Network.emulateNetworkConditions", "{\"offline\":false,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}");
        step(o, &n, "Network.setBlockedURLs", "{\"urls\":[]}");
        step(o, &n, "Network.disable", NULL);
    }
    sb_free(&p);
    return n;
}
void emu_plan_free(emu_step *st, int n) { for (int i = 0; i < n; i++) free(st[i].p); }

/* ---------------- dialog policy ---------------- */
static const char *const DLG_TYPES[] = {"alert", "confirm", "prompt", "beforeunload", NULL};
static const char *const DLG_DEF[] = {"accept", "dismiss", "dismiss", "accept"};

const char *dlg_action(const char *pol, const char *type) {
    const char *def = "accept";
    for (int i = 0; DLG_TYPES[i]; i++) if (!strcmp(type, DLG_TYPES[i])) def = DLG_DEF[i];
    if (!pol) return def;
    size_t tl = strlen(type);
    for (const char *s = pol; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
        if (!strncmp(s, type, tl) && s[tl] == '=') {
            const char *v = s + tl + 1;
            return !strncmp(v, "accept", 6) ? "accept" : !strncmp(v, "dismiss", 7) ? "dismiss" : "manual";
        }
    }
    return def;
}

int dlg_policy_merge(const char *old, const char *spec, char **out) {
    const char *act[4];
    for (int i = 0; i < 4; i++) act[i] = dlg_action(old, DLG_TYPES[i]);
    if (spec && strcmp(spec, "reset")) {
        char *cp = xstrdup(spec), *sv = NULL;
        for (char *t = strtok_r(cp, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
            char *eq = strchr(t, '=');
            if (!eq) { free(cp); return slim_seterr("bad dialog policy item '%s' (TYPE=accept|dismiss|manual)", t); }
            *eq++ = 0;
            const char *v = !strcmp(eq, "accept") ? "accept" : !strcmp(eq, "dismiss") ? "dismiss" : !strcmp(eq, "manual") ? "manual" : NULL;
            if (!v) { free(cp); return slim_seterr("bad dialog action '%s' (accept|dismiss|manual)", eq); }
            int hit = 0;
            for (int i = 0; i < 4; i++) if (!strcmp(t, "all") || !strcmp(t, DLG_TYPES[i])) { act[i] = v; hit = 1; }
            if (!hit) { free(cp); return slim_seterr("bad dialog type '%s' (alert|confirm|prompt|beforeunload|all)", t); }
        }
        free(cp);
    }
    sbuf b = {0};
    sb_printf(&b, "beforeunload=%s,alert=%s,confirm=%s,prompt=%s", act[3], act[0], act[1], act[2]);
    *out = b.p;
    return 0;
}

/* ---------------- files and process identity ---------------- */
static int hpath(const char *session, const char *ext, char *buf) { return state_path(session, ext, buf, PATH_MAX); }

static int proc_start(int pid, unsigned long long *start) {
    char p[64], *d; size_t n;
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    if (read_file(p, &d, &n, 4096)) return -1;
    char *e = strrchr(d, ')');
    int ok = 0;
    if (e && e[1] == ' ' && e[2] != 'Z' && e[2] != 'X') {
        char *q = e + 2;
        for (int f = 3; f < 22 && q; f++) { q = strchr(q, ' '); if (q) q++; }
        if (q) { *start = strtoull(q, NULL, 10); ok = 1; }
    }
    free(d);
    return ok ? 0 : -1;
}

/* pid is exactly our keeper: same start time, a slimium executable, argv "__hold --session S" */
static int keeper_valid(int pid, unsigned long long start, const char *session) {
    unsigned long long st;
    if (pid <= 0 || proc_start(pid, &st) || st != start) return 0;
    char p[64], exe[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t l = readlink(p, exe, sizeof exe - 1);
    if (l <= 0) return 0;
    exe[l] = 0;
    char *del = strstr(exe, " (deleted)");
    if (del) *del = 0;
    const char *b = path_base(exe);
    if (strncmp(b, "slimium", 7)) return 0;
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    char *cl; size_t n;
    if (read_file(p, &cl, &n, 4096)) return 0;
    const char *a[8]; int k = 0;
    for (size_t i = 0; i < n && k < 8; i += strlen(cl + i) + 1) a[k++] = cl + i;
    int ok = k == 4 && !strcmp(a[1], "__hold") && !strcmp(a[2], "--session") && !strcmp(a[3], session);
    free(cl);
    return ok;
}

typedef struct { int pid, gen, req_ok; unsigned long long start; long long req; char err[200]; } hfile;

static int hold_read(const char *session, hfile *h) {
    char p[PATH_MAX], *d; size_t n; jv doc;
    memset(h, 0, sizeof *h);
    if (hpath(session, ".hold", p) || read_file(p, &d, &n, 1 << 16)) return -1;
    int rc = -1;
    if (json_parse(d, n, &doc) == 0) {
        h->pid = (int)json_get_num(doc, "pid", 0);
        h->gen = (int)json_get_num(doc, "gen", -1);
        jv v;
        if (json_get(doc, "start", &v)) h->start = strtoull(v.p, NULL, 10);
        if (json_get(doc, "req", &v)) h->req = json_int(v, 0);
        h->req_ok = (int)json_get_num(doc, "req_ok", 0);
        char *e = json_get_str(doc, "req_err");
        if (e) { snprintf(h->err, sizeof h->err, "%s", e); free(e); }
        rc = 0;
    }
    free(d);
    return rc;
}

int hold_enabled(void) { const char *e = getenv("SLIM_HOLD"); return !(e && !strcmp(e, "0")); }

int hold_owner(const char *session, int *gen) {
    hfile h;
    if (hold_read(session, &h) || !keeper_valid(h.pid, h.start, session)) return 0;
    if (gen) *gen = h.gen;
    return h.pid;
}

int hold_spawn(const char *session, int wait_ms) {
    int pid = hold_owner(session, NULL);
    if (pid || !hold_enabled()) return pid;
    char self[PATH_MAX], logp[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return 0;
    self[l] = 0;
    if (access(self, X_OK)) snprintf(self, sizeof self, "/proc/self/exe");
    if (hpath(session, ".holdlog", logp)) return 0;
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) return 0;
    pid_t c = fork();
    if (c < 0) { close(pp[0]); close(pp[1]); return 0; }
    if (c == 0) {
        pid_t gc = fork();
        if (gc != 0) { ssize_t w = write(pp[1], &gc, sizeof gc); (void)w; _exit(0); }
        int nul = open("/dev/null", O_RDWR), lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(lf >= 0 ? lf : nul, 2); }
        setsid();
        signal(SIGHUP, SIG_IGN);
        closefrom(3);
        char *av[] = {"slimium", "__hold", "--session", (char *)session, NULL};
        execv(self, av);
        _exit(127);
    }
    close(pp[1]);
    pid_t gc = -1;
    if (read(pp[0], &gc, sizeof gc) != (ssize_t)sizeof gc) gc = -1;
    close(pp[0]);
    while (waitpid(c, NULL, 0) < 0 && errno == EINTR) {}
    for (long long dl = now_ms() + wait_ms; now_ms() < dl; sleep_ms(5)) {
        int g = -1;
        pid = hold_owner(session, &g);
        if (pid && g >= 0) return pid;
        unsigned long long st;
        if (gc > 0 && proc_start(gc, &st) && !pid) break;   /* it exited (lost the lock or no browser) */
    }
    return hold_owner(session, NULL);
}

int hold_ready(ctx_t *c) {
    if (c->port || !c->have_st || c->hold_none || !hold_enabled()) return 1;
    int gen = -1, pid = hold_owner(c->session, &gen);
    if (!pid) {
        pid = hold_spawn(c->session, 3000);
        if (!pid) { c->hold_none = 1; return 1; }
        hold_owner(c->session, &gen);
    }
    c->hold_pid = pid;
    if (gen >= c->st.gen) return 0;
    kill(pid, SIGUSR1);
    for (long long dl = now_ms() + WAIT_MS; now_ms() < dl; sleep_ms(5)) {
        if (!hold_owner(c->session, &gen)) { c->hold_pid = 0; return 1; }
        if (gen >= c->st.gen) return 0;
    }
    return slim_seterr("emulation keeper (pid %d) did not apply generation %d within %d ms", pid, c->st.gen, WAIT_MS);
}

int hold_request(ctx_t *c, const char *op, const char *target, int accept, const char *text) {
    int pid = c->hold_pid ? c->hold_pid : hold_owner(c->session, NULL);
    if (!pid) return slim_seterr("no emulation keeper for session '%s'", c->session);
    long long id = now_ms() * 1000 + getpid() % 1000;
    sbuf b = {0};
    sb_printf(&b, "{\"id\":%lld,\"op\":", id); sb_jstrz(&b, op);
    sb_puts(&b, ",\"target\":"); sb_jstrz(&b, target);
    sb_printf(&b, ",\"accept\":%s,\"text\":", accept ? "true" : "false"); sb_jstrz(&b, text);
    sb_puts(&b, "}\n");
    char p[PATH_MAX];
    int r = hpath(c->session, ".holdreq", p) || write_file(p, b.p, b.n, 1);
    sb_free(&b);
    if (r) return -1;
    kill(pid, SIGUSR1);
    for (long long dl = now_ms() + WAIT_MS; now_ms() < dl; sleep_ms(5)) {
        hfile h;
        if (hold_read(c->session, &h) || !keeper_valid(h.pid, h.start, c->session)) return slim_seterr("emulation keeper exited");
        if (h.req == id) return h.req_ok ? 0 : slim_seterr("%s", h.err[0] ? h.err : "keeper request failed");
    }
    return slim_seterr("emulation keeper (pid %d) did not answer within %d ms", pid, WAIT_MS);
}

int hold_stop(const char *session) {
    hfile h;
    if (hold_read(session, &h) || !keeper_valid(h.pid, h.start, session)) return 0;
    kill(h.pid, SIGTERM);
    long long dl = now_ms() + 2000;
    while (keeper_valid(h.pid, h.start, session) && now_ms() < dl) sleep_ms(10);
    if (keeper_valid(h.pid, h.start, session)) kill(h.pid, SIGKILL);
    return 1;
}

void hold_cleanup(const char *session) {
    static const char *const ext[] = {".hold", ".holdreq", ".pending", ".holdlock", ".holdlog", ".dialogs", NULL};
    char p[PATH_MAX];
    for (int i = 0; ext[i]; i++) if (hpath(session, ext[i], p) == 0) unlink(p);
}

int hold_pending(const char *session, const char *tid, char *desc, size_t n) {
    char p[PATH_MAX], *d; size_t len; struct stat sb;
    if (hpath(session, ".pending", p) || stat(p, &sb) || read_file(p, &d, &len, 1 << 20)) return 0;
    jv doc, e; size_t pos = 0; int hit = 0;
    if (json_parse(d, len, &doc) == 0)
        while (!hit && json_next(doc, &pos, NULL, &e)) {
            if (tid && !json_streq_path(e, "target", tid)) continue;
            char *ty = json_get_str(e, "type"), *m = json_get_str(e, "message");
            snprintf(desc, n, "a JavaScript %s dialog \"%.120s\" is open%s (dialog policy: manual); run 'slimium dialog accept|dismiss'",
                     ty ? ty : "?", m ? m : "", tid ? " on this tab" : "");
            free(ty); free(m); hit = 1;
        }
    free(d);
    return hit;
}

int dlg_log_count(const char *session, long *size) {
    char p[PATH_MAX], *d; size_t n;
    *size = 0;
    if (hpath(session, ".dialogs", p) || read_file(p, &d, &n, 1 << 20)) return 0;
    int k = 0;
    for (size_t i = 0; i < n; i++) if (d[i] == '\n') k++;
    *size = (long)n;
    free(d);
    return k;
}

/* ---------------- the keeper ---------------- */
typedef struct { char *sid, *tid; int net, dlg; char *dtype, *dmsg, *durl; } hpage;
typedef struct {
    const char *session;
    slim_state st;
    cdp_t cdp;
    hpage *pg; int npg, cap;
    long *ids; int nids, idcap;
    int want_ack, ack_gen, acked;
    long long ack_dl;
    char *sig, *ua, *ver;
    long long req_seen, req_done; long req_cmd; int req_ok; char req_err[200];
    unsigned long long start;
} hold_t;

static volatile sig_atomic_t h_stop;
static int h_wfd = -1;
static void h_sig(int s) {
    if (s != SIGUSR1) h_stop = 1;
    int e = errno; ssize_t w = write(h_wfd, "x", 1); (void)w; errno = e;
}

static void h_write(hold_t *h) {
    char p[PATH_MAX];
    if (hpath(h->session, ".hold", p)) return;
    sbuf b = {0};
    sb_printf(&b, "{\"pid\":%d,\"start\":%llu,\"gen\":%d,\"pages\":%d,\"req\":%lld,\"req_ok\":%d,\"req_err\":",
              (int)getpid(), h->start, h->acked, h->npg, h->req_done, h->req_ok);
    sb_jstrz(&b, h->req_err); sb_puts(&b, ",\"ua\":"); sb_jstrz(&b, h->ua);
    sb_puts(&b, ",\"version\":"); sb_jstrz(&b, h->ver); sb_puts(&b, "}\n");
    write_file(p, b.p, b.n, 1);
    sb_free(&b);
}

static void track(hold_t *h, long id) {
    if (h->nids == h->idcap) { h->idcap = h->idcap ? h->idcap * 2 : 32; h->ids = xrealloc(h->ids, sizeof *h->ids * (size_t)h->idcap); }
    h->ids[h->nids++] = id;
}
static void h_send(hold_t *h, const char *sid, const char *m, const char *p, int tr) {
    long id;
    if (cdp_send(&h->cdp, sid, m, p, &id) == 0 && tr) track(h, id);
}

static void apply_page(hold_t *h, hpage *pg, int full, int clear) {
    emu_step st[EMU_MAX];
    int n = emu_plan(&h->st, h->ua, h->ver, full, clear, pg->net, st);
    for (int i = 0; i < n; i++) h_send(h, pg->sid, st[i].m, st[i].p, 1);
    emu_plan_free(st, n);
    pg->net = h->st.offline || h->st.block;
}

static char *plan_sig(hold_t *h) {
    emu_step st[EMU_MAX];
    int n = emu_plan(&h->st, h->ua, h->ver, 1, 0, 1, st);
    sbuf b = {0};
    for (int i = 0; i < n; i++) { sb_puts(&b, st[i].m); sb_puts(&b, st[i].p); }
    emu_plan_free(st, n);
    if (!b.p) sb_puts(&b, "");
    return b.p;
}

static void begin_ack(hold_t *h) { h->want_ack = 1; h->ack_gen = h->st.gen; h->ack_dl = now_ms() + ACK_MS; }

static hpage *page_by(hold_t *h, const char *sid, const char *tid) {
    for (int i = 0; i < h->npg; i++)
        if ((sid && !strcmp(h->pg[i].sid, sid)) || (tid && !strcmp(h->pg[i].tid, tid))) return &h->pg[i];
    return NULL;
}

static void cut(sbuf *b, const char *k, const char *v, size_t max) {
    sb_puts(b, k);
    if (!v) { sb_puts(b, "null"); return; }
    size_t l = strlen(v);
    if (l > max) { l = max; while (l && ((unsigned char)v[l] & 0xC0) == 0x80) l--; }
    sb_jstr(b, v, l);
}

static void dlg_log(hold_t *h, hpage *pg, const char *type, const char *msg, const char *url, const char *action) {
    char p[PATH_MAX];
    if (hpath(h->session, ".dialogs", p)) return;
    struct stat sb;
    if (stat(p, &sb) == 0 && sb.st_size > LOG_MAX) {   /* keep the newer half */
        char *d; size_t n;
        if (read_file(p, &d, &n, 1 << 20) == 0) {
            char *s = memchr(d + n / 2, '\n', n - n / 2);
            if (s) write_file(p, s + 1, (size_t)(d + n - s - 1), 1);
            free(d);
        }
    }
    sbuf b = {0};
    sb_printf(&b, "{\"time\":%lld", (long long)(time(NULL)) * 1000);
    cut(&b, ",\"type\":", type, 32); cut(&b, ",\"message\":", msg, 200); cut(&b, ",\"url\":", url, 300);
    cut(&b, ",\"action\":", action, 64); cut(&b, ",\"target\":", pg ? pg->tid : NULL, 64);
    sb_puts(&b, "}\n");
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) { ssize_t w = write(fd, b.p, b.n); (void)w; close(fd); }
    sb_free(&b);
}

static void pending_write(hold_t *h) {
    char p[PATH_MAX];
    if (hpath(h->session, ".pending", p)) return;
    sbuf b = {0};
    int n = 0;
    sb_putc(&b, '[');
    for (int i = 0; i < h->npg; i++) {
        hpage *g = &h->pg[i];
        if (!g->dlg) continue;
        if (n++) sb_putc(&b, ',');
        cut(&b, "{\"target\":", g->tid, 64); cut(&b, ",\"type\":", g->dtype, 32);
        cut(&b, ",\"message\":", g->dmsg, 200); cut(&b, ",\"url\":", g->durl, 300); sb_putc(&b, '}');
    }
    sb_puts(&b, "]\n");
    if (n) write_file(p, b.p, b.n, 1); else unlink(p);
    sb_free(&b);
}

static void page_dlg_clear(hpage *g) {
    g->dlg = 0;
    free(g->dtype); free(g->dmsg); free(g->durl);
    g->dtype = g->dmsg = g->durl = NULL;
}

static void on_dialog(hold_t *h, const char *sid, jv pr) {
    hpage *g = page_by(h, sid, NULL);
    char *type = json_get_str(pr, "type"), *msg = json_get_str(pr, "message"), *url = json_get_str(pr, "url");
    const char *act = dlg_action(h->st.dlg, type ? type : "alert");
    if (!strcmp(act, "manual") && g) {
        page_dlg_clear(g);
        g->dlg = 1; g->dtype = type; g->dmsg = msg; g->durl = url;
        dlg_log(h, g, type, msg, url, "pending");
        pending_write(h);
        return;
    }
    if (!strcmp(act, "manual")) act = "accept";
    int acc = !strcmp(act, "accept");
    sbuf p = {0};
    sb_printf(&p, "{\"accept\":%s", acc ? "true" : "false");
    if (acc && type && !strcmp(type, "prompt")) {
        char *dp = json_get_str(pr, "defaultPrompt");
        sb_puts(&p, ",\"promptText\":"); sb_jstrz(&p, dp ? dp : ""); free(dp);
    }
    sb_putc(&p, '}');
    h_send(h, sid, "Page.handleJavaScriptDialog", p.p, 0);
    sb_free(&p);
    dlg_log(h, g, type, msg, url, act);
    free(type); free(msg); free(url);
}

static void on_request(hold_t *h) {
    char p[PATH_MAX], *d; size_t n; jv doc;
    if (hpath(h->session, ".holdreq", p) || read_file(p, &d, &n, 1 << 16)) return;
    if (json_parse(d, n, &doc)) { free(d); return; }
    long long id = (long long)json_get_num(doc, "id", 0);
    if (!id || id == h->req_seen) { free(d); return; }
    h->req_seen = id;
    h->req_err[0] = 0; h->req_ok = 0;
    if (json_streq_path(doc, "op", "reassert")) {
        for (int i = 0; i < h->npg; i++) apply_page(h, &h->pg[i], 1, 1);
        begin_ack(h);
        h->req_ok = 1; h->req_done = id;
    } else if (json_streq_path(doc, "op", "dialog")) {
        char *t = json_get_str(doc, "target"), *tx = json_get_str(doc, "text");
        hpage *g = NULL;
        for (int i = 0; i < h->npg && !g; i++) if (h->pg[i].dlg && (!t || !strcmp(h->pg[i].tid, t))) g = &h->pg[i];
        if (!g) { snprintf(h->req_err, sizeof h->req_err, "no JavaScript dialog is open%s", t ? " on this tab" : ""); h->req_done = id; }
        else {
            jv a; int acc = json_get(doc, "accept", &a) && json_bool(a);
            sbuf q = {0};
            sb_printf(&q, "{\"accept\":%s", acc ? "true" : "false");
            if (tx) { sb_puts(&q, ",\"promptText\":"); sb_jstrz(&q, tx); }
            sb_putc(&q, '}');
            long cid;
            if (cdp_send(&h->cdp, g->sid, "Page.handleJavaScriptDialog", q.p, &cid) == 0) { h->req_cmd = cid; h->req_done = 0; }
            else { snprintf(h->req_err, sizeof h->req_err, "%s", slim_errmsg()); h->req_done = id; }
            sb_free(&q);
            dlg_log(h, g, g->dtype, g->dmsg, g->durl, acc ? "accept (manual)" : "dismiss (manual)");
            page_dlg_clear(g);
            pending_write(h);
            h->req_seen = id;
        }
        free(t); free(tx);
    }
    free(d);
    if (h->req_done) h_write(h);
}

static void on_signal(hold_t *h) {
    slim_state s;
    if (state_load(h->session, &s)) { h_stop = 1; return; }
    state_free(&h->st);
    h->st = s;
    char *sig = plan_sig(h);
    int same = h->sig && !strcmp(sig, h->sig);
    free(h->sig); h->sig = sig;
    if (!same) for (int i = 0; i < h->npg; i++) apply_page(h, &h->pg[i], 1, 0);
    begin_ack(h);
    on_request(h);
}

static void on_message(hold_t *h, char *msg, size_t len) {
    jv doc, v, pr;
    if (json_parse(msg, len, &doc) || doc.t != J_OBJ) return;
    if (json_get(doc, "id", &v)) {
        long id = (long)json_int(v, -1);
        for (int i = 0; i < h->nids; i++) if (h->ids[i] == id) { h->ids[i] = h->ids[--h->nids]; break; }
        if (h->req_cmd && id == h->req_cmd) {
            jv e;
            h->req_ok = !json_get(doc, "error", &e);
            if (!h->req_ok) { char *m = json_get_str(e, "message"); snprintf(h->req_err, sizeof h->req_err, "%s", m ? m : "DevTools error"); free(m); }
            h->req_done = h->req_seen; h->req_cmd = 0;
            h_write(h);
        }
        return;
    }
    if (!json_get(doc, "method", &v)) return;
    if (!json_get(doc, "params", &pr)) { pr.p = "{}"; pr.n = 2; pr.t = J_OBJ; }
    char *sid = json_get_str(doc, "sessionId");
    if (json_str_eq(v, "Target.attachedToTarget")) {
        char *ns = json_get_str(pr, "sessionId"), *tid = json_get_str(pr, "targetInfo.targetId");
        if (ns && tid && json_streq_path(pr, "targetInfo.type", "page")) {
            if (h->npg == h->cap) { h->cap = h->cap ? h->cap * 2 : 8; h->pg = xrealloc(h->pg, sizeof *h->pg * (size_t)h->cap); }
            hpage *g = &h->pg[h->npg++];
            memset(g, 0, sizeof *g);
            g->sid = ns; g->tid = tid; ns = tid = NULL;
            h_send(h, g->sid, "Page.enable", NULL, 1);
            apply_page(h, g, 0, 0);
            h_send(h, g->sid, "Runtime.runIfWaitingForDebugger", NULL, 0);
        } else if (ns) h_send(h, ns, "Runtime.runIfWaitingForDebugger", NULL, 0);
        free(ns); free(tid);
    } else if (json_str_eq(v, "Target.detachedFromTarget")) {
        char *ds = json_get_str(pr, "sessionId");
        hpage *g = ds ? page_by(h, ds, NULL) : NULL;
        if (g) {
            int had = g->dlg;
            page_dlg_clear(g); free(g->sid); free(g->tid);
            *g = h->pg[--h->npg];
            if (had) pending_write(h);
        }
        free(ds);
    } else if (json_str_eq(v, "Page.javascriptDialogOpening") && sid) {
        on_dialog(h, sid, pr);
    } else if (json_str_eq(v, "Page.javascriptDialogClosed") && sid) {
        hpage *g = page_by(h, sid, NULL);
        if (g && g->dlg) {
            jv r; int ok = json_get(pr, "result", &r) && json_bool(r);
            dlg_log(h, g, g->dtype, g->dmsg, g->durl, ok ? "closed: accepted" : "closed: dismissed");
            page_dlg_clear(g); pending_write(h);
        }
    }
    free(sid);
}

static int hold_connect(hold_t *h) {
    int port = h->st.port;
    char *url = NULL, *resp; jv res;
    if (!(h->st.ws && cdp_connect(&h->cdp, h->st.ws, 3000) == 0)) {
        if (cdp_browser_url(port, 3000, &url, NULL) || cdp_connect(&h->cdp, url, 3000)) { free(url); return -1; }
        free(url);
    }
    if (cdp_call(&h->cdp, NULL, "Browser.getVersion", NULL, 5000, &resp, &res) == 0) {
        h->ua = json_get_str(res, "userAgent");
        char *pr = json_get_str(res, "product"), *sl = pr ? strchr(pr, '/') : NULL;
        if (sl) h->ver = xstrdup(sl + 1);
        free(pr); free(resp);
    }
    h->sig = plan_sig(h);
    begin_ack(h);
    return cdp_call(&h->cdp, NULL, "Target.setAutoAttach",
                    "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true,\"filter\":[{\"type\":\"page\",\"exclude\":false}]}",
                    5000, NULL, NULL);
}

int hold_main(ctx_t *c, int argc, char **argv) {
    (void)argv;
    if (argc || c->port) return usage_err(c, "__hold is internal: slimium --session S __hold");
    char p[PATH_MAX];
    if (hpath(c->session, ".holdlock", p)) return -1;
    int lk = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) return 0;     /* another keeper owns the session */
    hold_t *h = calloc(1, sizeof *h);
    if (!h) return slim_seterr("out of memory");
    h->session = c->session;
    h->acked = -1;
    if (state_load(c->session, &h->st)) { free(h); return -1; }
    proc_start(getpid(), &h->start);
    int pf[2];
    if (pipe2(pf, O_CLOEXEC | O_NONBLOCK)) return slim_seterr("pipe: %s", strerror(errno));
    h_wfd = pf[1];
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_sig; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int rc = 0;
    if (hold_connect(h)) { rc = slim_seterr("keeper: %s", slim_errmsg()); goto out; }
    h_write(h);
    long long next_check = now_ms() + 15000;
    while (!h_stop) {
        int avail = h->cdp.head || h->cdp.w.rpos < h->cdp.w.rlen;
        struct pollfd fds[2] = {{h->cdp.w.fd, POLLIN, 0}, {pf[0], POLLIN, 0}};
        if (!avail) {
            int to = h->want_ack ? 50 : 15000;
            if (poll(fds, 2, to) < 0 && errno != EINTR) break;
        }
        if (fds[1].revents & POLLIN) { char b[64]; while (read(pf[0], b, sizeof b) > 0) {} if (!h_stop) on_signal(h); }
        if (avail || (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char *m; size_t n;
            int r = cdp_next(&h->cdp, 5000, &m, &n);
            if (r < 0) break;           /* browser gone */
            if (r) { on_message(h, m, n); free(m); }
        }
        if (h->want_ack && (!h->nids || now_ms() > h->ack_dl) && !h->cdp.head) {
            h->want_ack = 0; h->nids = 0; h->acked = h->ack_gen;
            h_write(h);
        }
        if (now_ms() > next_check) {   /* session stopped or replaced */
            slim_state s;
            if (state_load(h->session, &s)) break;
            int same = s.port == h->st.port && s.pid == h->st.pid;
            state_free(&s);
            if (!same) break;
            next_check = now_ms() + 15000;
        }
    }
out:
    if (hold_owner(h->session, NULL) == getpid() && hpath(h->session, ".hold", p) == 0) unlink(p);
    for (int i = 0; i < h->npg; i++) h->pg[i].dlg = 0;
    h->npg = 0; pending_write(h);
    cdp_close(&h->cdp);
    return rc;
}

/* ---------------- 'dialogs' command ---------------- */
static int cmd_dialogs(ctx_t *c, int argc, char **argv) {
    int clear = opt_flag(&argc, argv, "--clear");
    const char *ls = opt_val(c, &argc, argv, "--last");
    if (opt_done(c, &argc, argv)) return -1;
    long last = 20;
    if (argc || (ls && (parse_int(ls, &last) || last < 1 || last > 100000))) return usage_err(c, "dialogs [--last N] [--clear]");
    if (c->port) return usage_err(c, "dialogs works on sessions (--session), not --port");
    char p[PATH_MAX], *d = NULL; size_t n = 0;
    if (hpath(c->session, ".dialogs", p)) return -1;
    if (read_file(p, &d, &n, 1 << 20)) { d = NULL; n = 0; }
    long total = 0;
    for (size_t i = 0; i < n; i++) if (d[i] == '\n') total++;
    size_t st = 0;
    for (long skip = total - last; skip > 0 && st < n; st++) if (d[st] == '\n') skip--;
    sb_puts(&c->oj, ",\"dialogs\":[");
    int m = 0;
    for (size_t i = st; i < n;) {
        char *e = memchr(d + i, '\n', n - i);
        size_t l = e ? (size_t)(e - d - i) : n - i;
        jv doc;
        if (l && json_parse(d + i, l, &doc) == 0) {
            if (m++) sb_putc(&c->oj, ',');
            sb_add(&c->oj, d + i, l);
            char *ty = json_get_str(doc, "type"), *ac = json_get_str(doc, "action"), *ms = json_get_str(doc, "message"), *u = json_get_str(doc, "url");
            out_t(c, "%s%s\t%s\t%s\t%s", m > 1 ? "\n" : "", ty ? ty : "", ac ? ac : "", ms ? ms : "", u ? u : "");
            free(ty); free(ac); free(ms); free(u);
        }
        i += l + 1;
    }
    sb_putc(&c->oj, ']');
    free(d);
    char desc[300];
    int pend = hold_pending(c->session, NULL, desc, sizeof desc);
    out_b(c, "pending", pend);
    if (pend) out_t(c, "%spending: %s", m ? "\n" : "", desc);
    if (!m && !pend) out_t(c, "no dialogs");
    if (clear) unlink(p);
    return 0;
}

static int cmd_hold(ctx_t *c, int argc, char **argv) { return hold_main(c, argc, argv); }

static const cmd_def HOLD_COMMANDS[] = {
    {"dialogs", cmd_dialogs, 0, "[--last N] [--clear]  JavaScript dialogs seen by the session (handled + pending)"},
    {"__hold", cmd_hold, CF_NOBATCH, "internal per-session emulation/dialog keeper"},
};

const cmd_def *slim_hold_commands(size_t *count) {
    *count = sizeof HOLD_COMMANDS / sizeof HOLD_COMMANDS[0];
    return HOLD_COMMANDS;
}
