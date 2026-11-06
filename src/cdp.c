#include "cdp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 4096
#define MAX_EVBYTES ((size_t)64 << 20)

int cdp_browser_url(int port, int timeout_ms, char **url, char **product) {
    char *body = NULL; size_t len = 0;
    int st = http_get("127.0.0.1", port, "/json/version", timeout_ms, &body, &len);
    if (st < 0) return slim_seterr("no DevTools endpoint on port %d (%s)", port, slim_errmsg());
    jv d;
    if (st != 200 || json_parse(body, len, &d)) { free(body); return slim_seterr("bad /json/version response on port %d", port); }
    char *u = json_get_str(d, "webSocketDebuggerUrl");
    if (product) *product = json_get_str(d, "Browser");
    free(body);
    if (!u) return slim_seterr("no webSocketDebuggerUrl on port %d", port);
    *url = u;
    return 0;
}

int cdp_connect(cdp_t *c, const char *ws_url, int timeout_ms) {
    memset(c, 0, sizeof *c);
    c->w.fd = -1;
    c->next_id = 1;
    const char *dbg = getenv("SLIM_DEBUG");
    c->debug = dbg && *dbg && strcmp(dbg, "0");
    if (ws_connect(&c->w, ws_url, timeout_ms)) return -1;
    c->open = 1;
    return 0;
}

void cdp_close(cdp_t *c) {
    if (c->open) ws_close(&c->w);
    c->open = 0;
    cdp_ev *e = c->head;
    while (e) { cdp_ev *n = e->next; free(e->msg); free(e); e = n; }
    c->head = c->tail = NULL; c->nev = c->evbytes = 0;
}

int cdp_send(cdp_t *c, const char *sid, const char *method, const char *params, long *id) {
    if (!c->open) return slim_seterr("not connected to browser");
    sbuf b = {0};
    long i = c->next_id++;
    sb_printf(&b, "{\"id\":%ld,\"method\":", i);
    sb_jstrz(&b, method);
    if (sid) { sb_puts(&b, ",\"sessionId\":"); sb_jstrz(&b, sid); }
    sb_puts(&b, ",\"params\":");
    sb_puts(&b, params && *params ? params : "{}");
    sb_putc(&b, '}');
    if (c->debug) fprintf(stderr, ">> %.*s\n", b.n > 400 ? 400 : (int)b.n, b.p);
    int r = ws_send(&c->w, WS_TEXT, b.p, b.n, 30000);
    sb_free(&b);
    if (r) { c->open = 0; return -1; }
    if (id) *id = i;
    return 0;
}

static void push_event(cdp_t *c, char *msg, size_t len) {
    cdp_ev *e = xmalloc(sizeof *e);
    e->next = NULL; e->msg = msg; e->len = len;
    if (c->tail) c->tail->next = e; else c->head = e;
    c->tail = e; c->nev++; c->evbytes += len;
    while (c->head && (c->nev > MAX_EVENTS || c->evbytes > MAX_EVBYTES)) {
        cdp_ev *h = c->head;
        c->head = h->next; if (!c->head) c->tail = NULL;
        c->nev--; c->evbytes -= h->len;
        free(h->msg); free(h);
    }
}

static int method_in(jv m, const char *methods) {
    if (!methods) return 1;
    if (m.t != J_STR) return 0;
    const char *mp = m.p + 1; size_t ml = m.n - 2;
    const char *s = methods;
    for (;;) {
        const char *bar = strchr(s, '|');
        size_t l = bar ? (size_t)(bar - s) : strlen(s);
        if (l == ml && !memcmp(s, mp, l)) return 1;
        if (!bar) return 0;
        s = bar + 1;
    }
}
static int sid_match(jv doc, const char *sid) {
    if (sid && !strcmp(sid, "*")) return 1;
    jv s;
    int has = json_get(doc, "sessionId", &s);
    if (!sid) return !has;
    return has && json_str_eq(s, sid);
}

/* receive one message: 1 ok, 0 timeout, -1 err */
static int recv_msg(cdp_t *c, long long dl, char **msg, size_t *len, jv *doc) {
    for (;;) {
        long long left = dl - now_ms();
        if (left < 0) left = 0;
        int r = ws_recv(&c->w, (int)left, msg, len);
        if (r <= 0) { if (r < 0) c->open = 0; return r; }
        if (c->debug) fprintf(stderr, "<< %.*s\n", *len > 400 ? 400 : (int)*len, *msg);
        if (json_parse(*msg, *len, doc) || doc->t != J_OBJ) { free(*msg); continue; }
        return 1;
    }
}

static int detached_check(jv doc, const char *sid) {
    if (!sid) return 0;
    jv m;
    if (!json_get(doc, "method", &m)) return 0;
    if (json_str_eq(m, "Target.detachedFromTarget") && json_streq_path(doc, "params.sessionId", sid))
        return slim_seterr("target detached (tab closed?)");
    if (json_str_eq(m, "Inspector.targetCrashed") && sid_match(doc, sid))
        return slim_seterr("target crashed");
    return 0;
}

/* deadline for one receive: the hook runs at least every 100 ms */
static long long hook_dl(cdp_t *c, long long dl, int *err) {
    *err = 0;
    if (!c->hook) return dl;
    long long t = now_ms();
    if (t >= c->hook_next) { c->hook_next = t + 100; if (c->hook(c->hook_ud)) { *err = 1; return dl; } }
    return c->hook_next < dl ? c->hook_next : dl;
}

int cdp_next(cdp_t *c, int timeout_ms, char **msg, size_t *len) {
    if (c->head) {
        cdp_ev *e = c->head;
        c->head = e->next; if (!c->head) c->tail = NULL;
        c->nev--; c->evbytes -= e->len;
        *msg = e->msg; *len = e->len; free(e);
        return 1;
    }
    jv doc;
    return recv_msg(c, now_ms() + timeout_ms, msg, len, &doc);
}

/* a response that arrived while waiting for another id was queued: take it */
static int take_id(cdp_t *c, long id, char **msg, size_t *len) {
    cdp_ev *prev = NULL;
    for (cdp_ev *e = c->head; e; prev = e, e = e->next) {
        jv doc, idv;
        if (json_parse(e->msg, e->len, &doc) || !json_get(doc, "id", &idv) || json_int(idv, -1) != id) continue;
        if (prev) prev->next = e->next; else c->head = e->next;
        if (c->tail == e) c->tail = prev;
        c->nev--; c->evbytes -= e->len;
        *msg = e->msg; *len = e->len;
        free(e);
        return 1;
    }
    return 0;
}

int cdp_wait(cdp_t *c, long id, const char *sid, int timeout_ms, char **resp, jv *result) {
    long long dl = now_ms() + timeout_ms;
    for (;;) {
        char *msg; size_t len; jv doc, idv;
        int r = take_id(c, id, &msg, &len);
        if (r) json_parse(msg, len, &doc);
        else {
            int herr; long long rdl = hook_dl(c, dl, &herr);
            if (herr) return -1;
            r = recv_msg(c, rdl, &msg, &len, &doc);
            if (r == 0 && now_ms() < dl) continue;
        }
        if (r == 0) return slim_seterr("timeout after %d ms waiting for DevTools response", timeout_ms);
        if (r < 0) return slim_seterr("browser connection lost: %s", slim_errmsg());
        if (json_get(doc, "id", &idv) && json_int(idv, -1) == id) {
            jv err;
            if (json_get(doc, "error", &err)) {
                char *em = json_get_str(err, "message");
                char *ed = json_get_str(err, "data");
                slim_seterr("%s%s%s", em ? em : "DevTools error", ed ? ": " : "", ed ? ed : "");
                free(em); free(ed); free(msg);
                return -1;
            }
            jv res;
            if (!json_get(doc, "result", &res)) { res.p = "{}"; res.n = 2; res.t = J_OBJ; }
            if (resp) { *resp = msg; if (result) *result = res; }
            else { free(msg); if (result) { result->p = "{}"; result->n = 2; result->t = J_OBJ; } }
            return 0;
        }
        if (json_get(doc, "method", NULL)) {
            int d = detached_check(doc, sid);
            push_event(c, msg, len);
            if (d) return -1;
        } else push_event(c, msg, len); /* response to another (pipelined) command */
    }
}

int cdp_call(cdp_t *c, const char *sid, const char *method, const char *params, int timeout_ms, char **resp, jv *result) {
    long id;
    if (cdp_send(c, sid, method, params, &id)) return slim_seterr("%s: %s", method, slim_errmsg());
    if (cdp_wait(c, id, sid, timeout_ms, resp, result)) {
        return slim_seterr("%s: %s", method, slim_errmsg());
    }
    return 0;
}

static int take(cdp_t *c, const char *sid, const char *methods, char **msg, jv *params, jv *mout) {
    cdp_ev *prev = NULL;
    for (cdp_ev *e = c->head; e; prev = e, e = e->next) {
        jv doc, m;
        if (json_parse(e->msg, e->len, &doc) || !json_get(doc, "method", &m)) continue;
        if (!method_in(m, methods) || !sid_match(doc, sid)) continue;
        if (prev) prev->next = e->next; else c->head = e->next;
        if (c->tail == e) c->tail = prev;
        c->nev--; c->evbytes -= e->len;
        *msg = e->msg;
        if (params && !json_get(doc, "params", params)) { params->p = "{}"; params->n = 2; params->t = J_OBJ; }
        if (mout) *mout = m;
        free(e);
        return 1;
    }
    return 0;
}

int cdp_event(cdp_t *c, const char *sid, const char *methods, int timeout_ms, char **msg, jv *params, jv *mout) {
    if (take(c, sid, methods, msg, params, mout)) return 1;
    long long dl = now_ms() + timeout_ms;
    for (;;) {
        char *m; size_t len; jv doc, mv;
        int herr; long long rdl = hook_dl(c, dl, &herr);
        if (herr) return -1;
        int r = recv_msg(c, rdl, &m, &len, &doc);
        if (r == 0) { if (now_ms() < dl) continue; return 0; }
        if (r < 0) return slim_seterr("browser connection lost: %s", slim_errmsg());
        if (!json_get(doc, "method", &mv)) { free(m); continue; }
        if (method_in(mv, methods) && sid_match(doc, sid)) {
            *msg = m;
            if (params && !json_get(doc, "params", params)) { params->p = "{}"; params->n = 2; params->t = J_OBJ; }
            if (mout) *mout = mv;
            return 1;
        }
        int d = (sid && strcmp(sid, "*")) ? detached_check(doc, sid) : 0;
        push_event(c, m, len);
        if (d) return -1;
    }
}

void cdp_flush(cdp_t *c, const char *sid, const char *methods) {
    char *m;
    while (take(c, sid, methods, &m, NULL, NULL)) free(m);
}

int cdp_attach(cdp_t *c, const char *target_id, int timeout_ms, char **sid) {
    sbuf p = {0};
    sb_puts(&p, "{\"targetId\":"); sb_jstrz(&p, target_id); sb_puts(&p, ",\"flatten\":true}");
    char *resp; jv res;
    int r = cdp_call(c, NULL, "Target.attachToTarget", p.p, timeout_ms, &resp, &res);
    sb_free(&p);
    if (r) return -1;
    *sid = json_get_str(res, "sessionId");
    free(resp);
    if (!*sid) return slim_seterr("Target.attachToTarget: no sessionId");
    return 0;
}

void cdp_detach(cdp_t *c, const char *sid, int timeout_ms) {
    if (!sid || !c->open) return;
    sbuf p = {0};
    sb_puts(&p, "{\"sessionId\":"); sb_jstrz(&p, sid); sb_putc(&p, '}');
    cdp_call(c, NULL, "Target.detachFromTarget", p.p, timeout_ms, NULL, NULL);
    sb_free(&p);
    cdp_flush(c, sid, NULL);
}
