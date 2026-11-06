/* Input: real mouse/keyboard events via Input.dispatch*Event, plus form helpers. */
#include "cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CENTER_JS "if(e.scrollIntoViewIfNeeded)e.scrollIntoViewIfNeeded(true);else e.scrollIntoView({block:'center',inline:'center'});" \
                  "var r=e.getBoundingClientRect();if(!(r.width>0&&r.height>0))throw new Error('element not visible: '+s);"

/* Scroll element into view and get its center in viewport CSS px. */
static int el_center(ctx_t *c, const char *sel, const char *extra, double *x, double *y, jv *extra_out, char **resp) {
    sbuf b = {0}, body = {0};
    sb_puts(&body, CENTER_JS "return [r.left+r.width/2,r.top+r.height/2");
    if (extra) { sb_putc(&body, ','); sb_puts(&body, extra); }
    sb_puts(&body, "]");
    js_el(&b, sel, body.p);
    sb_free(&body);
    char *r; jv o, v;
    int rc = ctx_eval(c, b.p, &r, &o);
    sb_free(&b);
    if (rc) return -1;
    if (!json_get(o, "value", &v) || v.t != J_ARR) { free(r); return slim_seterr("could not locate %s", sel); }
    *x = json_get_num(v, "0", 0); *y = json_get_num(v, "1", 0);
    if (extra_out) json_get(v, "2", extra_out);
    if (resp) *resp = r; else free(r);
    return 0;
}

static int mouse(ctx_t *c, const char *type, double x, double y, const char *button, int count) {
    char p[256];
    int buttons = !strcmp(type, "mousePressed") && button && !strcmp(button, "left") ? 1 : 0;
    snprintf(p, sizeof p, "{\"type\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d}",
             type, x, y, button ? button : "none", buttons, count);
    return cdp_call(&c->cdp, c->sid, "Input.dispatchMouseEvent", p, c->timeout, NULL, NULL);
}

static int click_at(ctx_t *c, double x, double y, int clicks) {
    if (mouse(c, "mouseMoved", x, y, NULL, 0)) return -1;
    for (int i = 1; i <= clicks; i++)
        if (mouse(c, "mousePressed", x, y, "left", i) || mouse(c, "mouseReleased", x, y, "left", i)) return -1;
    return 0;
}

static int click_cmd(ctx_t *c, int argc, char **argv, int clicks, const char *name) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "%s SELECTOR", name);
    double x, y;
    if (el_center(c, argv[0], NULL, &x, &y, NULL, NULL)) return -1;
    if (clicks ? click_at(c, x, y, clicks) : mouse(c, "mouseMoved", x, y, NULL, 0)) return -1;
    out_d(c, "x", x); out_d(c, "y", y);
    out_t(c, "%s %s at %.0f,%.0f", clicks == 0 ? "hovered" : clicks == 1 ? "clicked" : "double-clicked", argv[0], x, y);
    return 0;
}
int cmd_click(ctx_t *c, int argc, char **argv) { return click_cmd(c, argc, argv, 1, "click"); }
int cmd_dblclick(ctx_t *c, int argc, char **argv) { return click_cmd(c, argc, argv, 2, "dblclick"); }
int cmd_hover(ctx_t *c, int argc, char **argv) { return click_cmd(c, argc, argv, 0, "hover"); }

/* ---- keyboard ---- */
typedef struct { const char *name, *key, *code; int vk; const char *text; } keydef;
static const keydef KEYS[] = {
    {"Enter", "Enter", "Enter", 13, "\r"}, {"Return", "Enter", "Enter", 13, "\r"},
    {"Tab", "Tab", "Tab", 9, NULL}, {"Escape", "Escape", "Escape", 27, NULL}, {"Esc", "Escape", "Escape", 27, NULL},
    {"Backspace", "Backspace", "Backspace", 8, NULL}, {"Delete", "Delete", "Delete", 46, NULL},
    {"Insert", "Insert", "Insert", 45, NULL}, {"Space", " ", "Space", 32, " "},
    {"ArrowLeft", "ArrowLeft", "ArrowLeft", 37, NULL}, {"ArrowUp", "ArrowUp", "ArrowUp", 38, NULL},
    {"ArrowRight", "ArrowRight", "ArrowRight", 39, NULL}, {"ArrowDown", "ArrowDown", "ArrowDown", 40, NULL},
    {"Left", "ArrowLeft", "ArrowLeft", 37, NULL}, {"Up", "ArrowUp", "ArrowUp", 38, NULL},
    {"Right", "ArrowRight", "ArrowRight", 39, NULL}, {"Down", "ArrowDown", "ArrowDown", 40, NULL},
    {"Home", "Home", "Home", 36, NULL}, {"End", "End", "End", 35, NULL},
    {"PageUp", "PageUp", "PageUp", 33, NULL}, {"PageDown", "PageDown", "PageDown", 34, NULL},
    {"Shift", "Shift", "ShiftLeft", 16, NULL}, {"Control", "Control", "ControlLeft", 17, NULL},
    {"Alt", "Alt", "AltLeft", 18, NULL}, {"Meta", "Meta", "MetaLeft", 91, NULL},
    {"F1", "F1", "F1", 112, NULL}, {"F2", "F2", "F2", 113, NULL}, {"F3", "F3", "F3", 114, NULL},
    {"F4", "F4", "F4", 115, NULL}, {"F5", "F5", "F5", 116, NULL}, {"F6", "F6", "F6", 117, NULL},
    {"F7", "F7", "F7", 118, NULL}, {"F8", "F8", "F8", 119, NULL}, {"F9", "F9", "F9", 120, NULL},
    {"F10", "F10", "F10", 121, NULL}, {"F11", "F11", "F11", 122, NULL}, {"F12", "F12", "F12", 123, NULL},
};

static int send_key(ctx_t *c, const char *type, const char *key, const char *code, int vk, const char *text, int mods, long *id) {
    sbuf p = {0};
    sb_printf(&p, "{\"type\":\"%s\",\"modifiers\":%d,\"windowsVirtualKeyCode\":%d,\"key\":", type, mods, vk);
    sb_jstrz(&p, key);
    sb_puts(&p, ",\"code\":"); sb_jstrz(&p, code ? code : "");
    if (text) { sb_puts(&p, ",\"text\":"); sb_jstrz(&p, text); sb_puts(&p, ",\"unmodifiedText\":"); sb_jstrz(&p, text); }
    sb_putc(&p, '}');
    int r = cdp_send(&c->cdp, c->sid, "Input.dispatchKeyEvent", p.p, id);
    sb_free(&p);
    return r;
}

/* Describe a single character key. buf receives code. */
static void char_key(const char *ch, int *vk, char *code, size_t cn) {
    unsigned char k = (unsigned char)ch[0];
    *vk = 0; code[0] = 0;
    if (ch[1]) return; /* non-ASCII */
    if (k >= 'a' && k <= 'z') { *vk = k - 32; snprintf(code, cn, "Key%c", k - 32); }
    else if (k >= 'A' && k <= 'Z') { *vk = k; snprintf(code, cn, "Key%c", k); }
    else if (k >= '0' && k <= '9') { *vk = k; snprintf(code, cn, "Digit%c", k); }
    else if (k == ' ') { *vk = 32; snprintf(code, cn, "Space"); }
}

/* Queue keyDown(+text)/keyUp for one key; ids appended. */
static int key_pair(ctx_t *c, const keydef *kd, const char *ch, int mods, long *ids, int *n) {
    char code[16];
    if (kd) {
        const char *text = (mods & 7) ? NULL : kd->text;
        if (send_key(c, text ? "keyDown" : "rawKeyDown", kd->key, kd->code, kd->vk, text, mods, &ids[(*n)++])) return -1;
        return send_key(c, "keyUp", kd->key, kd->code, kd->vk, NULL, mods, &ids[(*n)++]);
    }
    int vk; char_key(ch, &vk, code, sizeof code);
    const char *text = (mods & 7) ? NULL : ch;
    if (send_key(c, text ? "keyDown" : "rawKeyDown", ch, code, vk, text, mods, &ids[(*n)++])) return -1;
    return send_key(c, "keyUp", ch, code, vk, NULL, mods, &ids[(*n)++]);
}

static int wait_ids(ctx_t *c, long *ids, int n) {
    int r = 0;
    for (int i = 0; i < n; i++) if (cdp_wait(&c->cdp, ids[i], c->sid, c->timeout, NULL, NULL) && !r) r = -1;
    return r;
}

static const keydef *find_key(const char *name) {
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (!strcasecmp(KEYS[i].name, name)) return &KEYS[i];
    return NULL;
}

int cmd_press(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "press KEY (Enter, Tab, Escape, ArrowDown, a, Control+a, ...)");
    char buf[128];
    if (strlen(argv[0]) >= sizeof buf) return usage_err(c, "key name too long");
    snprintf(buf, sizeof buf, "%s", argv[0]);
    int mods = 0;
    char *k = buf, *plus;
    /* modifiers: Alt=1 Control=2 Meta=4 Shift=8 ("+" alone is a key) */
    while ((plus = strchr(k, '+')) && plus[1]) {
        *plus = 0;
        if (!strcasecmp(k, "alt")) mods |= 1;
        else if (!strcasecmp(k, "control") || !strcasecmp(k, "ctrl")) mods |= 2;
        else if (!strcasecmp(k, "meta") || !strcasecmp(k, "cmd") || !strcasecmp(k, "super")) mods |= 4;
        else if (!strcasecmp(k, "shift")) mods |= 8;
        else return usage_err(c, "unknown modifier '%s'", k);
        k = plus + 1;
    }
    const keydef *kd = find_key(k);
    size_t kl = strlen(k);
    int one_char = kl > 0 && (kl == 1 || ((unsigned char)k[0] >= 0xC0 && kl <= 4));
    if (!kd && !one_char) return usage_err(c, "unknown key '%s'", k);
    if (ctx_page(c)) return -1;
    long ids[16]; int n = 0;
    static const keydef MODK[] = {{"Alt", "Alt", "AltLeft", 18, NULL}, {"Control", "Control", "ControlLeft", 17, NULL},
                                  {"Meta", "Meta", "MetaLeft", 91, NULL}, {"Shift", "Shift", "ShiftLeft", 16, NULL}};
    for (int i = 0; i < 4; i++)
        if (mods & (1 << i) && send_key(c, "rawKeyDown", MODK[i].key, MODK[i].code, MODK[i].vk, NULL, mods, &ids[n++])) return -1;
    char up[8] = {0};
    if (!kd && kl == 1 && (mods & 8) && k[0] >= 'a' && k[0] <= 'z') { up[0] = (char)(k[0] - 32); k = up; }
    if (key_pair(c, kd, k, mods, ids, &n)) return -1;
    for (int i = 3; i >= 0; i--)
        if (mods & (1 << i) && send_key(c, "keyUp", MODK[i].key, MODK[i].code, MODK[i].vk, NULL, 0, &ids[n++])) return -1;
    if (wait_ids(c, ids, n)) return -1;
    out_s(c, "key", argv[0]);
    out_t(c, "pressed %s", argv[0]);
    return 0;
}

static int focus_el(ctx_t *c, const char *sel, int clear) {
    sbuf b = {0};
    js_el(&b, sel, clear
        ? "e.scrollIntoViewIfNeeded&&e.scrollIntoViewIfNeeded(true);e.focus();"
          "if('value' in e&&e.tagName!=='SELECT'){e.select&&e.select();e.value='';e.dispatchEvent(new Event('input',{bubbles:true}))}"
          "else if(e.isContentEditable){var g=document.createRange();g.selectNodeContents(e);var w=getSelection();w.removeAllRanges();w.addRange(g);document.execCommand('delete')}"
          "return document.activeElement===e||e.contains(document.activeElement)"
        : "e.scrollIntoViewIfNeeded&&e.scrollIntoViewIfNeeded(true);e.focus();"
          "if(e.setSelectionRange&&typeof e.value==='string'){try{e.setSelectionRange(e.value.length,e.value.length)}catch(x){}}"
          "else if(e.isContentEditable){var g=document.createRange();g.selectNodeContents(e);g.collapse(false);var w=getSelection();w.removeAllRanges();w.addRange(g)}"
          "return document.activeElement===e||e.contains(document.activeElement)");
    char *r; jv o, v;
    int rc = ctx_eval(c, b.p, &r, &o);
    sb_free(&b);
    if (rc) return -1;
    int ok = json_get(o, "value", &v) && json_bool(v);
    free(r);
    if (!ok) return slim_seterr("element %s cannot be focused", sel);
    return 0;
}

int cmd_type(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "type SELECTOR TEXT");
    if (focus_el(c, argv[0], 0)) return -1;
    const char *s = argv[1];
    long ids[128]; int n = 0;
    size_t chars = 0;
    while (*s) {
        size_t l = 1;
        unsigned char u = (unsigned char)*s;
        if (u >= 0xF0) l = 4; else if (u >= 0xE0) l = 3; else if (u >= 0xC0) l = 2;
        for (size_t i = 1; i < l; i++) if (!s[i]) { l = i; break; }
        char ch[8]; memcpy(ch, s, l); ch[l] = 0;
        s += l; chars++;
        const keydef *kd = ch[0] == '\n' ? find_key("Enter") : ch[0] == '\t' ? find_key("Tab") : NULL;
        if (key_pair(c, kd, ch, 0, ids, &n)) return -1;
        if (n >= 120) { if (wait_ids(c, ids, n)) return -1; n = 0; }
    }
    if (wait_ids(c, ids, n)) return -1;
    out_s(c, "selector", argv[0]); out_i(c, "chars", (long long)chars);
    out_t(c, "typed %zu chars into %s", chars, argv[0]);
    return 0;
}

int cmd_fill(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "fill SELECTOR TEXT");
    if (focus_el(c, argv[0], 1)) return -1;
    if (*argv[1]) {
        sbuf p = {0};
        sb_puts(&p, "{\"text\":"); sb_jstrz(&p, argv[1]); sb_putc(&p, '}');
        int r = cdp_call(&c->cdp, c->sid, "Input.insertText", p.p, c->timeout, NULL, NULL);
        sb_free(&p);
        if (r) return -1;
    }
    sbuf b = {0};
    js_el(&b, argv[0], "e.dispatchEvent(new Event('change',{bubbles:true}));return e.type==='password'?undefined:'value' in e?e.value:e.innerText");
    char *r; jv o, v;
    int rc = ctx_eval(c, b.p, &r, &o);
    sb_free(&b);
    if (rc) return -1;
    if (json_get(o, "value", &v)) out_raw(c, "value", v.p, v.n);
    free(r);
    out_s(c, "selector", argv[0]);
    out_t(c, "filled %s", argv[0]);
    return 0;
}

int cmd_scroll(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    double dx, dy;
    if (argc != 2 || parse_double(argv[0], &dx) || parse_double(argv[1], &dy)) return usage_err(c, "scroll DX DY");
    char *r; jv o;
    if (ctx_eval(c, "[innerWidth/2,innerHeight/2,scrollX,scrollY]", &r, &o)) return -1;
    double x = json_get_num(o, "value.0", 100), y = json_get_num(o, "value.1", 100);
    double sx0 = json_get_num(o, "value.2", 0), sy0 = json_get_num(o, "value.3", 0);
    free(r);
    char p[200];
    snprintf(p, sizeof p, "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":%.1f,\"deltaY\":%.1f}", x, y, dx, dy);
    if (cdp_call(&c->cdp, c->sid, "Input.dispatchMouseEvent", p, c->timeout, NULL, NULL)) return -1;
    /* wheel scrolling is asynchronous (and may animate): wait until the position settles */
    double sx = sx0, sy = sy0;
    long long dl = now_ms() + 2000;
    int stable = 0;
    while (now_ms() < dl && stable < 3) {
        sleep_ms(30);
        if (ctx_eval(c, "[scrollX,scrollY]", &r, &o)) return -1;
        double nx = json_get_num(o, "value.0", 0), ny = json_get_num(o, "value.1", 0);
        free(r);
        int moved = nx != sx0 || ny != sy0;
        stable = (moved && nx == sx && ny == sy) ? stable + 1 : moved ? 0 : stable + (now_ms() > dl - 1700);
        sx = nx; sy = ny;
    }
    out_d(c, "scrollX", sx); out_d(c, "scrollY", sy);
    out_t(c, "scrolled to %.0f,%.0f", sx, sy);
    return 0;
}

int cmd_scroll_to(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "scroll-to SELECTOR");
    sbuf b = {0};
    js_el(&b, argv[0], "e.scrollIntoView({block:'center',inline:'nearest',behavior:'instant'});return [scrollX,scrollY]");
    char *r; jv o;
    int rc = ctx_eval(c, b.p, &r, &o);
    sb_free(&b);
    if (rc) return -1;
    double sx = json_get_num(o, "value.0", 0), sy = json_get_num(o, "value.1", 0);
    free(r);
    out_d(c, "scrollX", sx); out_d(c, "scrollY", sy);
    out_t(c, "scrolled to %.0f,%.0f", sx, sy);
    return 0;
}

int cmd_select(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "select SELECTOR VALUE");
    sbuf body = {0};
    sb_puts(&body, "var v=");
    sb_jstrz(&body, argv[1]);
    sb_puts(&body, ";if(e.tagName!=='SELECT')throw new Error(s+' is not a <select>');"
                   "var o=Array.prototype.find.call(e.options,function(x){return x.value===v})||"
                   "Array.prototype.find.call(e.options,function(x){return x.label===v||x.text.trim()===v});"
                   "if(!o)throw new Error('no option '+JSON.stringify(v)+' in '+s);"
                   "e.focus();o.selected=true;e.dispatchEvent(new Event('input',{bubbles:true}));"
                   "e.dispatchEvent(new Event('change',{bubbles:true}));return o.value");
    sbuf b = {0};
    js_el(&b, argv[0], body.p);
    sb_free(&body);
    char *r; jv o, v;
    int rc = ctx_eval(c, b.p, &r, &o);
    sb_free(&b);
    if (rc) return -1;
    if (json_get(o, "value", &v)) { out_raw(c, "value", v.p, v.n); out_t(c, "selected "); sb_jv_text(&c->ot, v); }
    free(r);
    return 0;
}

int cmd_check(ctx_t *c, int argc, char **argv) {
    int off = opt_flag(&argc, argv, "--off");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "check SELECTOR [--off]");
    sbuf b = {0};
    js_el(&b, argv[0], "if(!('checked' in e))throw new Error(s+' is not a checkbox/radio');return e.checked");
    char *r; jv o, v;
    int rc = ctx_eval(c, b.p, &r, &o);
    if (rc) { sb_free(&b); return -1; }
    int checked = json_get(o, "value", &v) && json_bool(v);
    free(r);
    int want = !off;
    if (checked != want) {
        double x, y;
        if (el_center(c, argv[0], NULL, &x, &y, NULL, NULL) == 0) {
            if (click_at(c, x, y, 1)) { sb_free(&b); return -1; }
        }
        rc = ctx_eval(c, b.p, &r, &o);
        if (rc) { sb_free(&b); return -1; }
        checked = json_get(o, "value", &v) && json_bool(v);
        free(r);
        if (checked != want) { /* covered or hidden input (custom widgets): fall back to a DOM click */
            sbuf b2 = {0};
            js_el(&b2, argv[0], "e.click();return e.checked");
            rc = ctx_eval(c, b2.p, &r, &o);
            sb_free(&b2);
            if (rc) { sb_free(&b); return -1; }
            checked = json_get(o, "value", &v) && json_bool(v);
            free(r);
        }
    }
    sb_free(&b);
    if (checked != want) return slim_seterr("could not %s %s", want ? "check" : "uncheck", argv[0]);
    out_b(c, "checked", checked);
    out_t(c, "%s %s", argv[0], checked ? "checked" : "unchecked");
    return 0;
}
