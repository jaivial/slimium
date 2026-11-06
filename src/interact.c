/* Fast, bounded page snapshots and universal locators. C11 + CDP only. */
#include "interact.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#define REF_PREFIX "SLIMIUM_REF_V1:"
#define MAX_REFS 4096
#define MAX_STATE (4u << 20)

static const char LOCATOR_JS[] =
"function N(x){return (x==null?'':String(x)).replace(/\\s+/g,' ').trim()}"
"function A(e){if(!e||!e.getAttribute)return '';var x=e.getAttribute('aria-label');if(x)return N(x);"
"var ids=e.getAttribute('aria-labelledby');if(ids){x=ids.split(/\\s+/).map(function(i){var q=document.getElementById(i);return q?N(q.textContent):''}).join(' ');if(N(x))return N(x)}"
"if(e.labels&&e.labels.length){x=Array.prototype.map.call(e.labels,function(q){return N(q.textContent)}).join(' ');if(N(x))return N(x)}"
"x=e.getAttribute('alt')||e.getAttribute('title');if(x)return N(x);"
"if(/^(INPUT|TEXTAREA|SELECT)$/.test(e.tagName)){x=e.getAttribute('placeholder');if(x)return N(x);"
"if(/^(submit|button|reset)$/.test(e.type))return N(e.value)}"
"return N(e.innerText!==undefined?e.innerText:e.textContent)}"
"function D(r){var a=[],seen=[];function w(q){if(!q||!q.querySelectorAll)return;var es=q.querySelectorAll('*');"
"for(var i=0;i<es.length;i++){var e=es[i];if(seen.indexOf(e)<0){seen.push(e);a.push(e)}if(e.shadowRoot)w(e.shadowRoot)}}"
"if(r.nodeType===1){a.push(r);if(r.shadowRoot)w(r.shadowRoot)}w(r);return a}"
"function Q(s){var exact=false,v=s;if(v.length>1&&v[0]==='\"'){try{v=JSON.parse(v);exact=true}catch(x){}}"
"else if(v.length>1&&v[0]===\"'\"&&v[v.length-1]===\"'\"){v=v.slice(1,-1);exact=true}return {v:N(v),exact:exact}}"
"function M(a,s){var q=Q(s),v=q.exact?q.v:q.v.toLowerCase();return a.some(function(e){var n=A(e);return q.exact?n===v:n.toLowerCase().indexOf(v)>=0})}"
"function L(r,s){var es,i;if(/^@e[1-9][0-9]*$/.test(s)){var re=window.__slimiumRefs&&window.__slimiumRefs[s];return re&&re.isConnected?[re]:[]}"
"if(/^css=/.test(s))return Array.prototype.slice.call(r.querySelectorAll(s.slice(4)));"
"if(/^(xpath=|\\/\\/|\\(\\/\\/)/.test(s)){var d=r.nodeType===9?r:r.ownerDocument,x=d.evaluate(s.indexOf('xpath=')===0?s.slice(6):s,r,null,7,null),a=[];"
"for(i=0;i<x.snapshotLength;i++)a.push(x.snapshotItem(i));return a}"
"es=D(r);if(s.indexOf('text=')===0){var tm=es.filter(function(e){return M([e],s.slice(5))});return tm.filter(function(e){return !tm.some(function(q){return q!==e&&e.contains(q)})})}"
"if(s.indexOf('role=')===0){var z=s.slice(5),m=z.match(/^([^\\[]+)(?:\\[name=(['\"])(.*?)\\2\\])?$/),role=m?m[1]:z,nm=m&&m[3];"
"return es.filter(function(e){var r=e.getAttribute('role')||({A:'link',BUTTON:'button',INPUT:e.type==='checkbox'?'checkbox':e.type==='radio'?'radio':'textbox',SELECT:'combobox',TEXTAREA:'textbox',OPTION:'option',IMG:'img',H1:'heading',H2:'heading',H3:'heading',H4:'heading',H5:'heading',H6:'heading'}[e.tagName]);"
"return r===role&&(!nm||A(e)===nm)})}"
"if(s.indexOf('testid=')===0){var id=s.slice(7);return es.filter(function(e){return e.getAttribute('data-testid')===id||e.getAttribute('data-test-id')===id||e.getAttribute('data-test')===id})}"
"if(s.indexOf('label=')===0){var q=Q(s.slice(6)),v=q.exact?q.v:q.v.toLowerCase();return es.filter(function(e){var n='';"
"if(e.labels)n=Array.prototype.map.call(e.labels,function(x){return N(x.textContent)}).join(' ');n=n||e.getAttribute('aria-label')||e.getAttribute('placeholder')||'';"
"return q.exact?N(n)===v:N(n).toLowerCase().indexOf(v)>=0})}"
"return Array.prototype.slice.call(r.querySelectorAll(s))}";

static int jobool(jv v, const char *path) { jv x; return json_get(v, path, &x) && json_bool(x); }

static const char *state_key(ctx_t *c, char buf[64]) {
    if (c->port) { snprintf(buf, 64, "port-%d", c->port); return buf; }
    return c->session;
}

typedef struct { long n; char *frame, *doc, *path; int main; } ref_ent;
typedef struct {
    char *target, *doc;
    long next;
    ref_ent *v;
    size_t n;
} ref_store;

static void store_free(ref_store *s) {
    free(s->target); free(s->doc);
    for (size_t i = 0; i < s->n; i++) { free(s->v[i].frame); free(s->v[i].doc); free(s->v[i].path); }
    free(s->v); memset(s, 0, sizeof *s);
}

static int store_path(ctx_t *c, char *p, size_t n) {
    char key[64];
    return state_path(state_key(c, key), ".interact.json", p, (unsigned)n);
}

static int store_load(ctx_t *c, ref_store *s) {
    memset(s, 0, sizeof *s);
    char p[4096]; if (store_path(c, p, sizeof p)) return -1;
    if (access(p, F_OK)) return 0;
    char *data; size_t len;
    if (read_file(p, &data, &len, MAX_STATE)) return -1;
    jv root;
    if (json_parse(data, len, &root) || root.t != J_OBJ) { free(data); return slim_seterr("corrupt interaction state"); }
    s->target = json_get_str(root, "target"); s->doc = json_get_str(root, "document");
    s->next = (long)json_get_num(root, "next", 1);
    jv a, x; size_t pos = 0;
    if (json_get(root, "refs", &a) && a.t == J_ARR) while (json_next(a, &pos, NULL, &x) && s->n < MAX_REFS) {
        ref_ent e; memset(&e, 0, sizeof e);
        e.n = (long)json_get_num(x, "n", 0); e.frame = json_get_str(x, "frame"); e.doc = json_get_str(x, "doc");
        e.path = json_get_str(x, "path"); jv bv; e.main = json_get(x, "main", &bv) && json_bool(bv);
        if (e.n > 0 && e.frame && e.doc) {
            s->v = xrealloc(s->v, (s->n + 1) * sizeof *s->v); s->v[s->n++] = e;
        } else { free(e.frame); free(e.doc); free(e.path); }
    }
    free(data); return 0;
}

static int store_save(ctx_t *c, const ref_store *s) {
    char p[4096], d[3072]; if (store_path(c, p, sizeof p)) return -1;
    if (state_dir(d, sizeof d)) return -1;
    if (chmod(d, 0700)) return slim_seterr("cannot protect runtime state directory");
    sbuf b = {0};
    sb_puts(&b, "{\"target\":"); sb_jstrz(&b, s->target);
    sb_puts(&b, ",\"document\":"); sb_jstrz(&b, s->doc);
    sb_printf(&b, ",\"next\":%ld,\"refs\":[", s->next);
    for (size_t i = 0; i < s->n; i++) {
        if (i) sb_putc(&b, ',');
        sb_printf(&b, "{\"n\":%ld,\"frame\":", s->v[i].n); sb_jstrz(&b, s->v[i].frame);
        sb_puts(&b, ",\"doc\":"); sb_jstrz(&b, s->v[i].doc);
        sb_puts(&b, ",\"path\":"); sb_jstrz(&b, s->v[i].path);
        sb_printf(&b, ",\"main\":%s}", s->v[i].main ? "true" : "false");
    }
    sb_puts(&b, "]}\n");
    int r = write_file(p, b.p, b.n, 1); sb_free(&b); return r;
}

static void store_add(ref_store *s, long n, const char *frame, const char *doc, const char *path, int main) {
    for (size_t i = 0; i < s->n; i++) if (s->v[i].n == n) {
        if (!strcmp(s->v[i].frame, frame) && !strcmp(s->v[i].doc, doc)) return;
        free(s->v[i].frame); free(s->v[i].doc); free(s->v[i].path);
        s->v[i].frame = xstrdup(frame); s->v[i].doc = xstrdup(doc); s->v[i].path = xstrdup(path); s->v[i].main = main; return;
    }
    if (s->n >= MAX_REFS) return;
    s->v = xrealloc(s->v, (s->n + 1) * sizeof *s->v);
    ref_ent *e = &s->v[s->n++]; e->n = n; e->frame = xstrdup(frame); e->doc = xstrdup(doc); e->path = xstrdup(path); e->main = main;
    if (n >= s->next) s->next = n + 1;
}

typedef struct { char *id, *parent, *url, *name, *path; int main, ctx; } frame_info;
typedef struct { frame_info *v; size_t n; } frame_list;

static void frames_free(frame_list *f) {
    for (size_t i = 0; i < f->n; i++) { free(f->v[i].id); free(f->v[i].parent); free(f->v[i].url); free(f->v[i].name); free(f->v[i].path); }
    free(f->v); memset(f, 0, sizeof *f);
}

static int frame_add(frame_list *f, jv node, const char *parent, const char *path, int main) {
    jv fr, kids; if (!json_get(node, "frame", &fr)) return 0;
    char *id = json_get_str(fr, "id"); if (!id) return 0;
    f->v = xrealloc(f->v, (f->n + 1) * sizeof *f->v);
    frame_info *x = &f->v[f->n++]; memset(x, 0, sizeof *x);
    x->id = id; x->parent = parent ? xstrdup(parent) : NULL; x->url = json_get_str(fr, "url"); x->name = json_get_str(fr, "name");
    x->path = xstrdup(path); x->main = main; x->ctx = 0;
    if (json_get(node, "childFrames", &kids)) {
        size_t pos = 0; jv k; int i = 0;
        while (json_next(kids, &pos, NULL, &k)) {
            char child[256]; snprintf(child, sizeof child, "%s/%d", path, i++);
            if (frame_add(f, k, id, child, 0)) return -1;
        }
    }
    return 0;
}

static int frames_get(ctx_t *c, frame_list *f) {
    memset(f, 0, sizeof *f);
    char *resp; jv res, tree;
    if (ctx_call(c, 1, "Page.getFrameTree", NULL, &resp, &res)) return -1;
    int r = 0;
    if (!json_get(res, "frameTree", &tree) || frame_add(f, tree, NULL, "0", 1)) r = slim_seterr("Page.getFrameTree: bad response");
    free(resp); return r;
}

typedef struct { int id; char *frame; } ctx_ent;
static struct { ctx_t *owner; char *sid; ctx_ent *v; size_t n; } contexts;

static void contexts_clear(void) {
    for (size_t i = 0; i < contexts.n; i++) free(contexts.v[i].frame);
    free(contexts.v); contexts.v = NULL; contexts.n = 0;
}

static int contexts_get(ctx_t *c) {
    if (ctx_enable(c, "Runtime")) return -1;
    if (contexts.owner != c || !contexts.sid || strcmp(contexts.sid, c->sid)) {
        contexts_clear(); free(contexts.sid); contexts.sid = xstrdup(c->sid); contexts.owner = c;
    }
    for (;;) {
        char *msg; jv p, m;
        int r = cdp_event(&c->cdp, c->sid, "Runtime.executionContextCreated|Runtime.executionContextDestroyed|Runtime.executionContextsCleared", 0, &msg, &p, &m);
        if (r < 0) return -1;
        if (!r) break;
        if (json_str_eq(m, "Runtime.executionContextsCleared")) contexts_clear();
        else if (json_str_eq(m, "Runtime.executionContextDestroyed")) {
            int id = (int)json_get_num(p, "executionContextId", 0);
            for (size_t i = 0; i < contexts.n; i++) if (contexts.v[i].id == id) {
                free(contexts.v[i].frame); contexts.v[i] = contexts.v[--contexts.n]; break;
            }
        } else if (jobool(p, "context.auxData.isDefault")) {
            int id = (int)json_get_num(p, "context.id", 0); char *fr = json_get_str(p, "context.auxData.frameId");
            if (id && fr) {
                size_t i; for (i = 0; i < contexts.n && contexts.v[i].id != id; i++) {}
                if (i == contexts.n) { contexts.v = xrealloc(contexts.v, (i + 1) * sizeof *contexts.v); contexts.n++; contexts.v[i].id = id; contexts.v[i].frame = NULL; }
                free(contexts.v[i].frame); contexts.v[i].frame = fr;
            } else free(fr);
        }
        free(msg);
    }
    return 0;
}

static int exception_result(jv res) {
    jv ex; if (!json_get(res, "exceptionDetails", &ex)) return 0;
    char *d = json_get_str(ex, "exception.description");
    if (!d) d = json_get_str(ex, "exception.value");
    if (!d) d = json_get_str(ex, "text");
    char *nl = d ? strchr(d, '\n') : NULL; if (nl) *nl = 0;
    slim_seterr("%s", d ? d : "JavaScript exception"); free(d); return -1;
}

static int eval_ctx(ctx_t *c, int context, const char *expr, int by_value, char **resp, jv *robj) {
    sbuf p = {0}; sb_puts(&p, "{\"expression\":"); sb_jstrz(&p, expr);
    sb_printf(&p, ",\"contextId\":%d,\"returnByValue\":%s,\"awaitPromise\":true,\"userGesture\":true}", context, by_value ? "true" : "false");
    char *r; jv res;
    int rc = cdp_call(&c->cdp, c->sid, "Runtime.evaluate", p.p, c->timeout, &r, &res); sb_free(&p);
    if (rc) return -1;
    if (exception_result(res)) { free(r); return -1; }
    if (!json_get(res, "result", robj)) { robj->p = "{}"; robj->n = 2; robj->t = J_OBJ; }
    if (resp) *resp = r; else free(r); return 0;
}

static int eval_main_mode(ctx_t *c, const char *expr, int by_value, char **resp, jv *robj) {
    if (by_value) return ctx_eval_in(c, c->sid, expr, resp, robj);
    sbuf p = {0}; sb_puts(&p, "{\"expression\":"); sb_jstrz(&p, expr);
    sb_puts(&p, ",\"returnByValue\":false,\"awaitPromise\":true,\"userGesture\":true}");
    char *r; jv res; int rc = cdp_call(&c->cdp, c->sid, "Runtime.evaluate", p.p, c->timeout, &r, &res); sb_free(&p);
    if (rc) return -1;
    if (exception_result(res)) { free(r); return -1; }
    if (!json_get(res, "result", robj)) { robj->p = "{}"; robj->n = 2; robj->t = J_OBJ; }
    if (resp) *resp = r; else free(r); return 0;
}

static int eval_frame_mode(ctx_t *c, frame_info *f, const char *expr, int by_value, char **resp, jv *robj) {
    if (f->main) return eval_main_mode(c, expr, by_value, resp, robj);
    if (!f->ctx) {
        if (contexts_get(c)) return -1;
        for (size_t i = 0; i < contexts.n; i++) if (!strcmp(contexts.v[i].frame, f->id)) { f->ctx = contexts.v[i].id; break; }
    }
    if (!f->ctx) return slim_seterr("frame %s has no accessible execution context", f->path);
    return eval_ctx(c, f->ctx, expr, by_value, resp, robj);
}

static int eval_frame(ctx_t *c, frame_info *f, const char *expr, char **resp, jv *robj) {
    return eval_frame_mode(c, f, expr, 1, resp, robj);
}

void interact_js_sel(sbuf *b, const char *sel, const char *body, int must) {
    if (!strncmp(sel, "@e", 2)) {
        sb_puts(b, REF_PREFIX "{\"ref\":"); sb_jstrz(b, sel); sb_puts(b, ",\"body\":"); sb_jstrz(b, body);
        sb_printf(b, ",\"must\":%s}", must ? "true" : "false"); return;
    }
    sb_puts(b, "(function(){var s="); sb_jstrz(b, sel); sb_puts(b, ";"); sb_puts(b, LOCATOR_JS);
    sb_puts(b, ";var a=L(document,s),e=a.length?a[0]:null;");
    if (must) sb_puts(b, "if(!e)throw new Error('no element matches selector '+s);");
    sb_puts(b, body); sb_puts(b, "})()");
}

static int frame_offset(ctx_t *c, const char *frame_id, double *ox, double *oy) {
    *ox = *oy = 0;
    frame_list frames;
    if (frames_get(c, &frames)) return -1;
    const char *cur = frame_id;
    int rc = 0, guard = 0;
    while (cur && guard++ < 64) {
        frame_info *f = NULL;
        for (size_t i = 0; i < frames.n; i++) if (!strcmp(frames.v[i].id, cur)) { f = &frames.v[i]; break; }
        if (!f || f->main || !f->parent) break;
        sbuf p = {0};
        sb_puts(&p, "{\"frameId\":");
        sb_jstrz(&p, cur);
        sb_putc(&p, '}');
        char *resp;
        jv res;
        if (cdp_call(&c->cdp, c->sid, "DOM.getFrameOwner", p.p, c->timeout, &resp, &res)) { sb_free(&p); rc = -1; break; }
        sb_free(&p);
        long backend = (long)json_get_num(res, "backendNodeId", 0);
        free(resp);
        if (!backend) { rc = slim_seterr("could not locate frame owner"); break; }
        char params[96];
        snprintf(params, sizeof params, "{\"backendNodeId\":%ld}", backend);
        if (cdp_call(&c->cdp, c->sid, "DOM.resolveNode", params, c->timeout, &resp, &res)) { rc = -1; break; }
        char *oid = json_get_str(res, "object.objectId");
        free(resp);
        if (!oid) { rc = slim_seterr("could not resolve frame owner"); break; }
        sb_puts(&p, "{\"objectId\":");
        sb_jstrz(&p, oid);
        sb_puts(&p, ",\"functionDeclaration\":\"function(){var r=this.getBoundingClientRect();return [r.left,r.top]}\",\"returnByValue\":true}");
        rc = cdp_call(&c->cdp, c->sid, "Runtime.callFunctionOn", p.p, c->timeout, &resp, &res);
        sb_free(&p);
        if (rc) { free(oid); break; }
        *ox += json_get_num(res, "result.value.0", 0);
        *oy += json_get_num(res, "result.value.1", 0);
        free(resp);
        sb_puts(&p, "{\"objectId\":");
        sb_jstrz(&p, oid);
        sb_putc(&p, '}');
        cdp_call(&c->cdp, c->sid, "Runtime.releaseObject", p.p, c->timeout, NULL, NULL);
        sb_free(&p);
        free(oid);
        cur = f->parent;
    }
    frames_free(&frames);
    return rc;
}

static char *absolute_rects(const char *body, double ox, double oy) {
    sbuf b = {0};
    while (*body) {
        if (!strncmp(body, "r.left", 6)) { sb_printf(&b, "(r.left+%.3f)", ox); body += 6; }
        else if (!strncmp(body, "r.top", 5)) { sb_printf(&b, "(r.top+%.3f)", oy); body += 5; }
        else if (!strncmp(body, "r.x", 3)) { sb_printf(&b, "(r.x+%.3f)", ox); body += 3; }
        else if (!strncmp(body, "r.y", 3)) { sb_printf(&b, "(r.y+%.3f)", oy); body += 3; }
        else sb_putc(&b, *body++);
    }
    return b.p;
}

static ref_ent *ref_lookup(ref_store *s, long n) {
    for (size_t i = 0; i < s->n; i++) if (s->v[i].n == n) return &s->v[i];
    return NULL;
}

static int eval_ref_mode(ctx_t *c, const char *expr, int by_value, char **resp, jv *robj) {
    if (strncmp(expr, REF_PREFIX, strlen(REF_PREFIX))) return 0;
    const char *js = expr + strlen(REF_PREFIX); jv in;
    if (json_parse(js, strlen(js), &in)) return slim_seterr("bad internal ref expression");
    char *ref = json_get_str(in, "ref"), *body = json_get_str(in, "body");
    if (!ref || !body) { free(ref); free(body); return slim_seterr("bad internal ref request"); }
    char *end; long n = strtol(ref + 2, &end, 10);
    if (strncmp(ref, "@e", 2) || n <= 0 || *end) { int bad = slim_seterr("bad element ref '%s'", ref); free(ref); free(body); return bad; }
    if (ctx_page(c)) { free(ref); free(body); return -1; }
    ref_store s; if (store_load(c, &s)) { free(ref); free(body); return -1; }
    ref_ent *e = (!s.target || !strcmp(s.target, c->tid)) ? ref_lookup(&s, n) : NULL;
    if (!e) { store_free(&s); free(body); int r = slim_seterr("unknown ref %s for current tab; run snapshot or find again", ref); free(ref); return r; }
    char *absolute = NULL;
    if (!e->main && strstr(body, "getBoundingClientRect")) {
        double ox, oy;
        if (frame_offset(c, e->frame, &ox, &oy)) { store_free(&s); free(ref); free(body); return -1; }
        absolute = absolute_rects(body, ox, oy);
    }
    sbuf b = {0}; sb_puts(&b, "(function(){var s="); sb_jstrz(&b, ref); sb_puts(&b, ";if(window.__slimiumDoc!=="); sb_jstrz(&b, e->doc);
    sb_puts(&b, ")throw new Error('stale ref '+s+': document changed; run snapshot again');if(window.__slimiumURL!==location.href)throw new Error('stale ref '+s+': navigation occurred; run snapshot again');var e=window.__slimiumRefs&&window.__slimiumRefs[s];"
                "if(!e||!e.isConnected)throw new Error('stale ref '+s+': element is no longer attached; run snapshot again');");
    sb_puts(&b, absolute ? absolute : body); sb_puts(&b, "})()");
    free(absolute);
    int r;
    if (e->main) r = eval_main_mode(c, b.p, by_value, resp, robj);
    else {
        frame_info f; memset(&f, 0, sizeof f); f.id = e->frame; f.path = e->path; f.main = 0;
        r = eval_frame_mode(c, &f, b.p, by_value, resp, robj);
    }
    sb_free(&b); store_free(&s); free(ref); free(body); return r ? -1 : 1;
}

int interact_eval_ref(ctx_t *c, const char *expr, char **resp, jv *robj) {
    return eval_ref_mode(c, expr, 1, resp, robj);
}

static int eval_any_mode(ctx_t *c, const char *expr, int by_value, char **resp, jv *robj) {
    if (!strncmp(expr, REF_PREFIX, strlen(REF_PREFIX))) return eval_ref_mode(c, expr, by_value, resp, robj) < 0 ? -1 : 0;
    if (ctx_page(c)) return -1;
    return eval_main_mode(c, expr, by_value, resp, robj);
}

static int eval_locator(ctx_t *c, const char *locator, const char *body, char **resp, jv *robj) {
    sbuf b = {0}; interact_js_sel(&b, locator, body, 1); int r = ctx_eval(c, b.p, resp, robj); sb_free(&b); return r;
}

typedef struct { sbuf elements; size_t count; int truncated; } list_result;

static int collect(ctx_t *c, const char *find, const char *scope, int interactive, int all, int max, list_result *out) {
    memset(out, 0, sizeof *out);
    frame_list frames; if (frames_get(c, &frames)) return -1;
    ref_store store; if (store_load(c, &store)) { frames_free(&frames); return -1; }
    if (store.target && strcmp(store.target, c->tid)) { store_free(&store); memset(&store, 0, sizeof store); }
    if (!store.next || store.next < 1) store.next = 1;
    char *old_doc = store.doc ? xstrdup(store.doc) : NULL;
    sb_puts(&out->elements, "[");
    int rc = 0;
    for (size_t fi = 0; fi < frames.n && !out->truncated; fi++) {
        frame_info *f = &frames.v[fi];
        sbuf cfg = {0}; sb_printf(&cfg, "{\"base\":%ld,\"max\":%d,\"main\":%s,\"path\":", store.next, max - (int)out->count, f->main ? "true" : "false");
        sb_jstrz(&cfg, f->path); sb_puts(&cfg, ",\"expected\":");
        if (f->main && old_doc) sb_jstrz(&cfg, old_doc); else sb_puts(&cfg, "null");
        sb_puts(&cfg, ",\"find\":"); sb_jstrz(&cfg, find); sb_puts(&cfg, ",\"scope\":"); sb_jstrz(&cfg, scope);
        sb_printf(&cfg, ",\"interactive\":%s,\"all\":%s}", interactive ? "true" : "false", all ? "true" : "false");
        sbuf js = {0}; sb_puts(&js, "(function(cfg){"); sb_puts(&js, LOCATOR_JS);
        sb_puts(&js,
        "var w=window;if(!w.__slimiumDoc||(cfg.main&&cfg.expected&&w.__slimiumDoc!==cfg.expected)){w.__slimiumDoc=Date.now().toString(36)+Math.random().toString(36).slice(2);w.__slimiumRefs={};if(cfg.main)cfg.base=1}"
        "if(!w.__slimiumRefs)w.__slimiumRefs={};w.__slimiumURL=location.href;var refs=w.__slimiumRefs,next=cfg.base,a=[],trunc=false,roots=[document];"
        "if(cfg.scope){roots=L(document,cfg.scope).slice(0,1);if(!roots.length)return {doc:w.__slimiumDoc,next:next,elements:[],truncated:false}}"
        "var es=[];if(cfg.find){for(var ri=0;ri<roots.length;ri++)es=es.concat(L(roots[ri],cfg.find))}else for(ri=0;ri<roots.length;ri++)es=es.concat(D(roots[ri]));"
        "for(var i=0;i<es.length;i++){var e=es[i];if(!e||e.nodeType!==1)continue;var tag=e.tagName,role=e.getAttribute('role')||'';"
        "var it=/^(A|BUTTON|INPUT|SELECT|TEXTAREA|SUMMARY)$/.test(tag)||e.hasAttribute('onclick')||e.hasAttribute('tabindex')||/^(button|link|checkbox|radio|textbox|combobox|menuitem|tab)$/.test(role);"
        "if(cfg.interactive&&!it)continue;if(!cfg.find&&!cfg.all&&!it&&!/^(H[1-6]|IMG|FORM|LABEL|DIALOG|P)$/.test(tag))continue;"
        "if(a.length>=cfg.max){trunc=true;break}var ref=e.__slimiumRef;if(!ref||!/^@e[1-9][0-9]*$/.test(ref)){ref='@e'+next++;e.__slimiumRef=ref}refs[ref]=e;"
        "var r=e.getBoundingClientRect(),st=getComputedStyle(e),vis=r.width>0&&r.height>0&&st.display!=='none'&&st.visibility!=='hidden'&&st.visibility!=='collapse';"
        "var pw=tag==='INPUT'&&e.type==='password',o={ref:ref,role:role||({A:'link',BUTTON:'button',INPUT:e.type||'text',SELECT:'combobox',TEXTAREA:'textbox'}[tag]||tag.toLowerCase()),tag:tag.toLowerCase(),name:A(e).slice(0,120),frame:cfg.path,visible:vis,box:[Math.round(r.x),Math.round(r.y),Math.round(r.width),Math.round(r.height)]};"
        "var txt=N(e.innerText!==undefined?e.innerText:e.textContent);if(txt&&txt!==o.name)o.text=txt.slice(0,160);"
        "if(tag==='INPUT')o.type=e.type||'text';if(pw)o.password=true;else if('value' in e&&/^(INPUT|TEXTAREA|SELECT|OPTION)$/.test(tag))o.value=String(e.value).slice(0,120);"
        "if('checked' in e)o.checked=!!e.checked;if('disabled' in e)o.disabled=!!e.disabled;a.push(o)}"
        "return {doc:w.__slimiumDoc,next:next,elements:a,truncated:trunc}})(");
        sb_add(&js, cfg.p, cfg.n); sb_puts(&js, ")"); sb_free(&cfg);
        char *resp; jv obj, val;
        if (eval_frame(c, f, js.p, &resp, &obj)) { sb_free(&js); rc = -1; break; }
        sb_free(&js);
        if (!json_get(obj, "value", &val)) { free(resp); rc = slim_seterr("snapshot: no result"); break; }
        char *doc = json_get_str(val, "doc");
        if (!doc) { free(resp); rc = slim_seterr("snapshot: no document identity"); break; }
        if (f->main) {
            if (!store.doc || strcmp(store.doc, doc)) { free(store.doc); store.doc = xstrdup(doc); store.next = 1; }
            else { free(store.doc); store.doc = doc; }
        }
        long returned_next = (long)json_get_num(val, "next", store.next); if (returned_next > store.next) store.next = returned_next;
        jv arr, el; size_t pos = 0;
        if (json_get(val, "elements", &arr)) while (json_next(arr, &pos, NULL, &el)) {
            char *ref = json_get_str(el, "ref"); long n = ref ? strtol(ref + 2, NULL, 10) : 0;
            if (n > 0) {
                store_add(&store, n, f->id, doc, f->path, f->main);
                if (out->count++) sb_putc(&out->elements, ',');
                sb_add(&out->elements, el.p, el.n);
            }
            free(ref);
        }
        if (jobool(val, "truncated")) out->truncated = 1;
        if (f->main && store.doc != doc) free(doc); else if (!f->main) free(doc);
        free(resp);
    }
    sb_puts(&out->elements, "]");
    if (!rc && !store.target) store.target = xstrdup(c->tid);
    if (!rc && (!store.doc || !*store.doc)) rc = slim_seterr("snapshot: main frame unavailable");
    if (!rc) rc = store_save(c, &store);
    free(old_doc); store_free(&store); frames_free(&frames);
    if (rc) { sb_free(&out->elements); out->count = 0; }
    return rc;
}

static void emit_list(ctx_t *c, list_result *r) {
    out_i(c, "count", (long long)r->count); out_raw(c, "elements", r->elements.p, r->elements.n); out_b(c, "truncated", r->truncated);
    jv a, e; size_t pos = 0; size_t line = 0;
    if (json_parse(r->elements.p, r->elements.n, &a) == 0) while (json_next(a, &pos, NULL, &e)) {
        char *ref = json_get_str(e, "ref"), *role = json_get_str(e, "role"), *name = json_get_str(e, "name");
        char *type = json_get_str(e, "type"), *value = json_get_str(e, "value"), *frame = json_get_str(e, "frame");
        out_t(c, "%s%s %s ", line++ ? "\n" : "", ref ? ref : "", role ? role : ""); sb_jstrz(&c->ot, name ? name : "");
        if (type) out_t(c, " type=%s", type);
        if (value) { out_t(c, " value="); sb_jstrz(&c->ot, value); }
        jv v; if (json_get(e, "checked", &v)) out_t(c, " checked=%s", json_bool(v) ? "true" : "false");
        if (json_get(e, "password", &v)) out_t(c, " value=<redacted>");
        out_t(c, " visible=%s box=%lld,%lld,%lld,%lld frame=%s", jobool(e, "visible") ? "true" : "false",
              (long long)json_get_num(e, "box.0", 0), (long long)json_get_num(e, "box.1", 0),
              (long long)json_get_num(e, "box.2", 0), (long long)json_get_num(e, "box.3", 0), frame ? frame : "0");
        free(ref); free(role); free(name); free(type); free(value); free(frame);
    }
}

static int parse_max(ctx_t *c, int *argc, char **argv, int def) {
    const char *m = opt_val(c, argc, argv, "--max"); long n = def;
    if (m && (parse_int(m, &n) || n < 1 || n > 2000)) { usage_err(c, "--max must be 1..2000"); return def; }
    return (int)n;
}

int interact_snapshot(ctx_t *c, int argc, char **argv) {
    int interactive = opt_flag(&argc, argv, "--interactive"), all = opt_flag(&argc, argv, "--all");
    const char *scope = opt_val(c, &argc, argv, "--selector"); int max = parse_max(c, &argc, argv, 200);
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "snapshot [--interactive] [--all] [--selector S] [--max N]");
    list_result r; if (collect(c, NULL, scope, interactive, all, max, &r)) return -1;
    emit_list(c, &r); sb_free(&r.elements); return 0;
}

int interact_find(ctx_t *c, int argc, char **argv) {
    int max = parse_max(c, &argc, argv, 100);
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "find LOCATOR [--max N]");
    list_result r; if (collect(c, argv[0], NULL, 0, 1, max, &r)) return -1;
    emit_list(c, &r); sb_free(&r.elements); return 0;
}

int interact_inspect(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "inspect LOCATOR");
    list_result list; if (collect(c, argv[0], NULL, 0, 1, 2, &list)) return -1;
    if (list.count != 1) { sb_free(&list.elements); return slim_seterr("inspect needs exactly one match, got %zu", list.count); }
    jv a, el; if (json_parse(list.elements.p, list.elements.n, &a) || !json_next(a, &(size_t){0}, NULL, &el)) { sb_free(&list.elements); return slim_seterr("inspect: bad match"); }
    char *ref = json_get_str(el, "ref"), *frame = json_get_str(el, "frame"); sb_free(&list.elements);
    sbuf b = {0};
    interact_js_sel(&b, ref,
      "function U(e){if(e.id){var q='#'+CSS.escape(e.id);if(document.querySelectorAll(q).length===1)return q}"
      "var a=[];while(e&&e.nodeType===1&&e.tagName!=='HTML'){var p=e.parentElement;if(!p)break;var n=1,x=e;while((x=x.previousElementSibling))if(x.tagName===e.tagName)n++;"
      "a.unshift(e.tagName.toLowerCase()+':nth-of-type('+n+')');e=p}return a.join(' > ')}"
      "var r=e.getBoundingClientRect(),st=getComputedStyle(e),at={},pw=e.tagName==='INPUT'&&e.type==='password';"
      "for(var i=0;i<e.attributes.length;i++){var x=e.attributes[i];if(pw&&(x.name==='value'||x.name==='type'&&false))continue;at[x.name]=x.value}"
      "return {selector:U(e),attributes:at,box:{x:r.x,y:r.y,width:r.width,height:r.height},styles:{display:st.display,visibility:st.visibility,opacity:st.opacity,position:st.position,color:st.color,backgroundColor:st.backgroundColor,font:st.font,pointerEvents:st.pointerEvents}}", 1);
    char *resp; jv obj, val; int rc = ctx_eval(c, b.p, &resp, &obj); sb_free(&b);
    if (rc) { free(ref); free(frame); return -1; }
    if (json_get(obj, "value", &val)) {
        jv x;
        if (json_get(val, "selector", &x)) out_raw(c, "selector", x.p, x.n);
        if (json_get(val, "attributes", &x)) out_raw(c, "attributes", x.p, x.n);
        if (json_get(val, "box", &x)) out_raw(c, "box", x.p, x.n);
        if (json_get(val, "styles", &x)) out_raw(c, "styles", x.p, x.n);
        char *sel = json_get_str(val, "selector"); out_s(c, "ref", ref); out_s(c, "frame", frame); out_t(c, "%s\t%s", sel ? sel : "", ref); free(sel);
    }
    free(resp); free(ref); free(frame); return 0;
}


int interact_upload(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc < 2) return usage_err(c, "upload LOCATOR FILE...");
    sbuf b = {0}; interact_js_sel(&b, argv[0], "return e", 1);
    char *resp; jv obj;
    int rc = eval_any_mode(c, b.p, 0, &resp, &obj); sb_free(&b);
    if (rc) return -1;
    char *oid = json_get_str(obj, "objectId");
    if (!oid) { free(resp); return slim_seterr("upload locator did not resolve to a file input object"); }
    sbuf p = {0}; sb_puts(&p, "{\"files\":["); int files = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX]; struct stat st;
        if (!realpath(argv[i], path) || stat(path, &st) || !S_ISREG(st.st_mode) || access(path, R_OK)) {
            sb_free(&p); free(oid); free(resp); return slim_seterr("upload file is not readable: %s", argv[i]);
        }
        if (files++) sb_putc(&p, ',');
        sb_jstrz(&p, path);
    }
    sb_puts(&p, "],\"objectId\":"); sb_jstrz(&p, oid); sb_putc(&p, '}');
    rc = cdp_call(&c->cdp, c->sid, "DOM.setFileInputFiles", p.p, c->timeout, NULL, NULL);
    sb_free(&p);
    sbuf rel = {0}; sb_puts(&rel, "{\"objectId\":"); sb_jstrz(&rel, oid); sb_putc(&rel, '}');
    cdp_call(&c->cdp, c->sid, "Runtime.releaseObject", rel.p, c->timeout, NULL, NULL); sb_free(&rel);
    free(oid); free(resp);
    if (rc) return -1;
    out_s(c, "selector", argv[0]); out_i(c, "files", files); out_t(c, "uploaded %d files to %s", files, argv[0]); return 0;
}

static int locator_center(ctx_t *c, const char *locator, double *x, double *y) {
    char *resp; jv obj, val;
    int rc = eval_locator(c, locator,
      "if(e.scrollIntoViewIfNeeded)e.scrollIntoViewIfNeeded(true);else e.scrollIntoView({block:'center',inline:'center'});"
      "var r=e.getBoundingClientRect();if(!(r.width>0&&r.height>0))throw new Error('element not visible: '+s);"
      "return [r.left+r.width/2,r.top+r.height/2]", &resp, &obj);
    if (rc) return -1;
    if (!json_get(obj, "value", &val) || val.t != J_ARR) { free(resp); return slim_seterr("could not locate %s", locator); }
    *x = json_get_num(val, "0", 0); *y = json_get_num(val, "1", 0); free(resp); return 0;
}

static int imouse(ctx_t *c, const char *type, double x, double y, const char *button, int clicks) {
    char p[256]; snprintf(p, sizeof p, "{\"type\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d}",
                          type, x, y, button ? button : "none", button && !strcmp(button, "left") && !strcmp(type, "mousePressed") ? 1 : 0, clicks);
    return cdp_call(&c->cdp, c->sid, "Input.dispatchMouseEvent", p, c->timeout, NULL, NULL);
}

int interact_drag(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "drag FROM TO");
    double x0, y0, x1, y1;
    if (locator_center(c, argv[0], &x0, &y0) || locator_center(c, argv[1], &x1, &y1)) return -1;
    if (imouse(c, "mouseMoved", x0, y0, NULL, 0) || imouse(c, "mousePressed", x0, y0, "left", 1)) return -1;
    for (int i = 1; i <= 6; i++) if (imouse(c, "mouseMoved", x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6, "left", 0)) return -1;
    if (imouse(c, "mouseReleased", x1, y1, "left", 1)) return -1;
    out_s(c, "from", argv[0]); out_s(c, "to", argv[1]); out_t(c, "dragged %s to %s", argv[0], argv[1]); return 0;
}

int interact_focus(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "focus LOCATOR");
    char *resp; jv obj, val;
    int rc = eval_locator(c, argv[0],
      "if(e.scrollIntoViewIfNeeded)e.scrollIntoViewIfNeeded(true);else e.scrollIntoView({block:'center'});e.focus();"
      "return document.activeElement===e||e.contains(document.activeElement)", &resp, &obj);
    if (rc) return -1;
    int ok = json_get(obj, "value", &val) && json_bool(val); free(resp);
    if (!ok) return slim_seterr("element %s cannot be focused", argv[0]);
    out_s(c, "selector", argv[0]); out_t(c, "focused %s", argv[0]); return 0;
}

int interact_submit(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "submit LOCATOR");
    char *resp; jv obj;
    int rc = eval_locator(c, argv[0],
      "var f=e.tagName==='FORM'?e:e.closest('form');if(!f)throw new Error('no form associated with '+s);"
      "if(f.requestSubmit)f.requestSubmit();else f.submit();return true", &resp, &obj);
    if (rc) return -1;
    free(resp);
    out_s(c, "selector", argv[0]); out_t(c, "submitted %s", argv[0]); return 0;
}

static int invoke_capture(ctx_t *c, int argc, char **argv, sbuf *json) {
    sbuf oldj = c->oj, oldt = c->ot; int usage = c->usage;
    memset(&c->oj, 0, sizeof c->oj); memset(&c->ot, 0, sizeof c->ot);
    int rc = cli_invoke(c, argc, argv);
    *json = c->oj; sb_free(&c->ot); c->oj = oldj; c->ot = oldt; c->usage = usage; return rc;
}

static int read_json_input(ctx_t *c, const char *arg, char **data, size_t *len) {
    if (arg[0] == '@') return read_file(arg + 1, data, len, 16u << 20);
    if (!strcmp(arg, "-")) {
        if (c->in_batch) return usage_err(c, "stdin JSON is not available inside act/batch/pipe");
        return read_fd_all(0, data, len, 16u << 20);
    }
    *data = xstrdup(arg); *len = strlen(arg); return 0;
}

int interact_form(ctx_t *c, int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "fill")) { argc--; argv++; }
    else return usage_err(c, "form fill JSON|@file");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "form fill JSON|@file");
    char *data; size_t len; if (read_json_input(c, argv[0], &data, &len)) return -1;
    jv root; int bad = json_parse(data, len, &root) || root.t != J_OBJ;
    jv fields = root;
    if (!bad && json_get(root, "fields", &fields) && fields.t != J_OBJ) bad = 1;
    if (bad) { free(data); return slim_seterr("form fill expects a JSON object of LOCATOR: value"); }
    size_t pos = 0, done = 0; jv key, value;
    while (json_next(fields, &pos, &key, &value)) {
        char *loc = json_str(key); int rc = 0; sbuf captured = {0};
        if (value.t == J_BOOL) {
            char *av[4] = {"check", loc, NULL, NULL}; int ac = 2;
            if (!json_bool(value)) av[ac++] = "--off";
            rc = invoke_capture(c, ac, av, &captured);
        } else {
            char *text = json_str(value); char *tagresp; jv tagobj, tagval;
            rc = eval_locator(c, loc, "return e.tagName", &tagresp, &tagobj);
            int is_select = !rc && json_get(tagobj, "value", &tagval) && json_str_eq(tagval, "SELECT");
            free(tagresp);
            if (!rc) {
                char *av[4] = {is_select ? "select" : "fill", loc, text, NULL};
                rc = invoke_capture(c, 3, av, &captured);
            }
            free(text);
        }
        sb_free(&captured);
        if (rc) {
            char *why = xstrdup(slim_errmsg()); slim_seterr("form field %s: %s", loc, why);
            free(why); free(loc); free(data); return -1;
        }
        free(loc); done++;
    }
    free(data); out_i(c, "fields", (long long)done); out_t(c, "filled %zu fields", done); return 0;
}


typedef struct { char **v; int n, cap; } arg_list;
static void args_free(arg_list *a) { for (int i = 0; i < a->n; i++) free(a->v[i]); free(a->v); memset(a, 0, sizeof *a); }
static int args_add(arg_list *a, const char *s) {
    if (a->n >= 31) return slim_seterr("too many command arguments");
    if (a->n + 2 > a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = xrealloc(a->v, (size_t)a->cap * sizeof *a->v); }
    a->v[a->n++] = xstrdup(s); a->v[a->n] = NULL; return 0;
}
static int args_jv(arg_list *a, jv v) {
    if (v.t == J_OBJ || v.t == J_ARR || v.t == J_NONE || v.t == J_NULL) return slim_seterr("command arguments must be strings, numbers or booleans");
    char *s = json_str(v); int r = args_add(a, s); free(s); return r;
}

static char *field_string(jv obj, const char *a, const char *b, const char *c) {
    char *s = json_get_str(obj, a); if (!s && b) s = json_get_str(obj, b); if (!s && c) s = json_get_str(obj, c); return s;
}

static void action_result(sbuf *out, size_t index, const char *cmd, int rc, sbuf *captured, const char *error) {
    sb_printf(out, "%s{\"index\":%zu,\"action\":", index ? "," : "", index); sb_jstrz(out, cmd ? cmd : "");
    sb_printf(out, ",\"ok\":%s", rc ? "false" : "true");
    if (!rc && captured && captured->n) sb_add(out, captured->p, captured->n);
    if (rc) { sb_puts(out, ",\"error\":"); sb_jstrz(out, error ? error : "action failed"); }
    sb_putc(out, '}');
}

static int act_args(jv a, arg_list *av, char **cmd_out) {
    char *cmd = field_string(a, "action", "cmd", "op");
    if (!cmd) return slim_seterr("action needs 'action' or 'cmd'");
    *cmd_out = cmd;
    if (!strcmp(cmd, "act") || !strcmp(cmd, "pipe") || !strcmp(cmd, "mcp") || !strcmp(cmd, "batch"))
        return slim_seterr("recursive %s is not allowed in act", cmd);
    if (args_add(av, cmd)) return -1;
    jv args;
    if (json_get(a, "args", &args)) {
        if (args.t != J_ARR) return slim_seterr("action args must be an array");
        size_t pos = 0; jv v; while (json_next(args, &pos, NULL, &v)) if (args_jv(av, v)) return -1;
        return 0;
    }
    if (!strcmp(cmd, "eval")) {
        char *s = field_string(a, "js", "expression", "code"); if (!s) return slim_seterr("eval action needs js");
        int r = args_add(av, s); free(s); return r;
    }
    if (!strcmp(cmd, "press")) {
        char *s = json_get_str(a, "key"); if (!s) s = json_get_str(a, "value"); if (!s) return slim_seterr("press action needs key");
        int r = args_add(av, s); free(s); return r;
    }
    if (!strcmp(cmd, "nav")) {
        char *s = json_get_str(a, "url"); if (!s) return slim_seterr("nav action needs url");
        int r = args_add(av, s); free(s); return r;
    }
    if (!strcmp(cmd, "snapshot")) {
        if (jobool(a, "interactive") && args_add(av, "--interactive")) return -1;
        if (jobool(a, "all") && args_add(av, "--all")) return -1;
        jv m; if (json_get(a, "max", &m)) { char tmp[32]; snprintf(tmp, sizeof tmp, "%lld", json_int(m, 200)); if (args_add(av, "--max") || args_add(av, tmp)) return -1; }
        char *s = json_get_str(a, "selector"); if (s) { int r = args_add(av, "--selector") || args_add(av, s); free(s); if (r) return -1; }
        return 0;
    }
    char *loc = field_string(a, "locator", "selector", "target");
    if (!loc) loc = json_get_str(a, "ref");
    if (loc) { int r = args_add(av, loc); free(loc); if (r) return -1; }
    else if (strcmp(cmd, "wait-load") && strcmp(cmd, "wait-idle")) return slim_seterr("%s action needs locator or args", cmd);
    if (!strcmp(cmd, "fill") || !strcmp(cmd, "type") || !strcmp(cmd, "select")) {
        jv v; char *s = NULL;
        if (json_get(a, "value", &v) || json_get(a, "text", &v)) s = json_str(v);
        if (!s) return slim_seterr("%s action needs value", cmd);
        int r = args_add(av, s); free(s); if (r) return -1;
    }
    if (!strcmp(cmd, "check") && jobool(a, "off") && args_add(av, "--off")) return -1;
    if (!strcmp(cmd, "wait")) {
        if (jobool(a, "visible") && args_add(av, "--visible")) return -1;
        if (jobool(a, "gone") && args_add(av, "--gone")) return -1;
    }
    return 0;
}

int interact_act(ctx_t *c, int argc, char **argv) {
    int stop = opt_flag(&argc, argv, "--stop-on-error"), final_snap = opt_flag(&argc, argv, "--snapshot");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "act JSON|@file|- [--stop-on-error] [--snapshot]");
    char *data; size_t len; if (read_json_input(c, argv[0], &data, &len)) return -1;
    jv root; if (json_parse(data, len, &root) || root.t != J_ARR) { free(data); return slim_seterr("act expects a JSON array of actions"); }
    sbuf results = {0}; sb_putc(&results, '['); size_t pos = 0, index = 0, failures = 0, done = 0; jv a;
    while (json_next(root, &pos, NULL, &a)) {
        arg_list av = {0}; char *cmd = NULL; int rc = 0; sbuf captured = {0};
        if (a.t != J_OBJ) rc = slim_seterr("action must be an object");
        else rc = act_args(a, &av, &cmd);
        if (!rc) rc = invoke_capture(c, av.n, av.v, &captured);
        if (rc && (!strcmp(cmd, "click") || !strcmp(cmd, "fill") || !strcmp(cmd, "type") || !strcmp(cmd, "select") || !strcmp(cmd, "check")) && av.n > 1) {
            char *wav[5] = {"wait", av.v[1], "--visible", NULL};
            sbuf wait_out = {0};
            sb_free(&captured); memset(&captured, 0, sizeof captured);
            rc = invoke_capture(c, 3, wav, &wait_out);
            sb_free(&wait_out);
            if (!rc) rc = invoke_capture(c, av.n, av.v, &captured);
        }
        char *error = rc ? xstrdup(slim_errmsg()) : NULL;
        action_result(&results, index, cmd ? cmd : "", rc, &captured, error);
        if (rc) failures++;
        done++;
        free(error); sb_free(&captured); args_free(&av); free(cmd); index++;
        if (rc && stop) break;
    }
    sb_putc(&results, ']'); free(data);
    out_i(c, "actions", (long long)done); out_i(c, "failures", (long long)failures); out_raw(c, "results", results.p, results.n);
    out_t(c, "%zu actions, %zu failed", done, failures); sb_free(&results);
    if (final_snap) {
        char *sav[2] = {"snapshot", NULL}; sbuf snap = {0};
        int rc = invoke_capture(c, 1, sav, &snap);
        if (rc) { sb_free(&snap); return -1; }
        sb_puts(&c->oj, ",\"snapshot\":{"); if (snap.n > 1) sb_add(&c->oj, snap.p + 1, snap.n - 1); sb_putc(&c->oj, '}'); sb_free(&snap);
    }
    return 0;
}

static void pipe_response(jv *id, const char *cmd, int rc, sbuf *captured, const char *error) {
    sbuf b = {0}; sb_puts(&b, "{\"id\":"); if (id) sb_add(&b, id->p, id->n); else sb_puts(&b, "null");
    sb_printf(&b, ",\"ok\":%s", rc ? "false" : "true");
    if (cmd) { sb_puts(&b, ",\"cmd\":"); sb_jstrz(&b, cmd); }
    if (!rc && captured && captured->n) sb_add(&b, captured->p, captured->n);
    if (rc) { sb_puts(&b, ",\"error\":"); sb_jstrz(&b, error ? error : "request failed"); }
    sb_puts(&b, "}\n"); fwrite(b.p, 1, b.n, stdout); fflush(stdout); sb_free(&b);
}

static int pipe_request(ctx_t *c, jv root, arg_list *av, char **cmd, char **tab, int *timeout) {
    (void)c;
    jv args, v;
    size_t pos = 0;
    if (json_get(root, "argv", &args)) {
        if (args.t != J_ARR || !json_next(args, &pos, NULL, &v)) return slim_seterr("argv must be a nonempty array");
        *cmd = json_str(v);
        while (json_next(args, &pos, NULL, &v)) if (args_jv(av, v)) return -1;
    } else {
        *cmd = json_get_str(root, "cmd");
        if (!*cmd) return slim_seterr("request needs cmd or argv");
        if (json_get(root, "args", &args)) {
            if (args.t != J_ARR) return slim_seterr("args must be an array");
            while (json_next(args, &pos, NULL, &v)) if (args_jv(av, v)) return -1;
        }
    }
    if (!strcmp(*cmd, "pipe") || !strcmp(*cmd, "mcp") || !strcmp(*cmd, "batch")) return slim_seterr("recursive %s is not allowed in pipe", *cmd);
    if (args_add(av, *cmd)) return -1;
    char *last = av->v[av->n - 1];
    memmove(&av->v[1], &av->v[0], (size_t)(av->n - 1) * sizeof *av->v);
    av->v[0] = last;
    int end = 0;
    for (int i = 1; i < av->n; i++) {
        char *s = av->v[i];
        if (end || (strcmp(s, "--tab") && strncmp(s, "--tab=", 6) && strcmp(s, "--timeout") && strncmp(s, "--timeout=", 10) &&
            strcmp(s, "--json") && strcmp(s, "--session") && strncmp(s, "--session=", 10) && strcmp(s, "--port") && strncmp(s, "--port=", 7))) {
            if (!strcmp(s, "--")) end = 1;
            continue;
        }
        if (!strcmp(s, "--")) { end = 1; continue; }
        if (!strcmp(s, "--json")) { free(av->v[i]); av->v[i] = NULL; continue; }
        if (!strcmp(s, "--session") || !strncmp(s, "--session=", 10) || !strcmp(s, "--port") || !strncmp(s, "--port=", 7))
            return slim_seterr("--session/--port are fixed for a pipe");
        int is_tab = !strcmp(s, "--tab") || !strncmp(s, "--tab=", 6);
        const char *val = strchr(s, '=');
        int two = !val;
        if (val) val++;
        else if (i + 1 < av->n) val = av->v[i + 1];
        else return slim_seterr("%s needs a value", s);
        if (is_tab) {
            free(*tab);
            *tab = xstrdup(val);
        } else {
            long n;
            if (parse_int(val, &n) || n <= 0 || n > 86400000) return slim_seterr("bad timeout '%s'", val);
            *timeout = (int)n;
        }
        free(av->v[i]); av->v[i] = NULL;
        if (two) { free(av->v[++i]); av->v[i] = NULL; }
    }
    int dst = 1;
    for (int i = 1; i < av->n; i++) if (av->v[i]) av->v[dst++] = av->v[i];
    av->n = dst;
    av->v[dst] = NULL;
    return 0;
}

int interact_pipe(ctx_t *c, int argc, char **argv) {
    int stop = opt_flag(&argc, argv, "--stop-on-error");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "pipe [--stop-on-error]");
    char *line = NULL; size_t cap = 0; ssize_t len; int old_batch = c->in_batch;
    const char *base_tab = c->tab; int base_timeout = c->timeout, base_set = c->timeout_set; c->in_batch = 1;
    while ((len = getline(&line, &cap, stdin)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        jv root, id, *idp = NULL; int rc = 0; arg_list av = {0}; char *cmd = NULL;
        char *tab = NULL; int timeout = base_timeout;
        if (len > 1 << 20) rc = slim_seterr("request line too large");
        else if (json_parse(line, (size_t)len, &root) || root.t != J_OBJ) rc = slim_seterr("malformed NDJSON request");
        else {
            if (json_get(root, "id", &id)) idp = &id;
            rc = pipe_request(c, root, &av, &cmd, &tab, &timeout);
        }
        sbuf captured = {0};
        if (!rc) {
            int temporary = tab != NULL;
            c->timeout = timeout; c->timeout_set = 1; c->tab = tab ? tab : base_tab;
            if (temporary) ctx_reset_page(c);
            rc = invoke_capture(c, av.n, av.v, &captured);
            if (temporary) ctx_reset_page(c);
            c->tab = base_tab; c->timeout = base_timeout; c->timeout_set = base_set;
        }
        char *error = rc ? xstrdup(slim_errmsg()) : NULL;
        pipe_response(idp, cmd, rc, &captured, error);
        free(error); sb_free(&captured); args_free(&av); free(cmd); free(tab);
        if (rc && stop) break;
    }
    free(line); c->in_batch = old_batch; c->tab = base_tab; c->timeout = base_timeout; c->timeout_set = base_set;
    return 0;
}
