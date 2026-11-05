#include "json.h"
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 512

static const char *skip_ws(const char *s, const char *e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    return s;
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* s at opening quote; returns pointer past closing quote or NULL. */
static const char *skip_str(const char *s, const char *e) {
    s++;
    while (s < e) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') return s + 1;
        if (c < 0x20) return NULL;
        if (c == '\\') {
            if (++s >= e) return NULL;
            switch (*s) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': s++; break;
            case 'u':
                if (e - s < 5) return NULL;
                for (int i = 1; i <= 4; i++) if (hexv(s[i]) < 0) return NULL;
                s += 5; break;
            default: return NULL;
            }
        } else s++;
    }
    return NULL;
}
static const char *skip_num(const char *s, const char *e) {
    const char *st = s;
    if (s < e && *s == '-') s++;
    if (s >= e) return NULL;
    if (*s == '0') s++;
    else if (*s >= '1' && *s <= '9') while (s < e && *s >= '0' && *s <= '9') s++;
    else return NULL;
    if (s < e && *s == '.') {
        s++; const char *d = s;
        while (s < e && *s >= '0' && *s <= '9') s++;
        if (s == d) return NULL;
    }
    if (s < e && (*s == 'e' || *s == 'E')) {
        s++; if (s < e && (*s == '+' || *s == '-')) s++;
        const char *d = s;
        while (s < e && *s >= '0' && *s <= '9') s++;
        if (s == d) return NULL;
    }
    return s > st ? s : NULL;
}
static const char *skip_lit(const char *s, const char *e, const char *lit) {
    size_t l = strlen(lit);
    if ((size_t)(e - s) < l || memcmp(s, lit, l)) return NULL;
    return s + l;
}
static const char *skip_val(const char *s, const char *e, int depth, jtype *t) {
    if (depth > MAX_DEPTH || s >= e) return NULL;
    switch (*s) {
    case '"': *t = J_STR; return skip_str(s, e);
    case 't': *t = J_BOOL; return skip_lit(s, e, "true");
    case 'f': *t = J_BOOL; return skip_lit(s, e, "false");
    case 'n': *t = J_NULL; return skip_lit(s, e, "null");
    case '[': case '{': {
        int obj = *s == '{';
        char close = obj ? '}' : ']';
        *t = obj ? J_OBJ : J_ARR;
        s = skip_ws(s + 1, e);
        if (s < e && *s == close) return s + 1;
        for (;;) {
            jtype tt;
            if (obj) {
                if (s >= e || *s != '"') return NULL;
                s = skip_str(s, e);
                if (!s) return NULL;
                s = skip_ws(s, e);
                if (s >= e || *s != ':') return NULL;
                s = skip_ws(s + 1, e);
            }
            s = skip_val(s, e, depth + 1, &tt);
            if (!s) return NULL;
            s = skip_ws(s, e);
            if (s >= e) return NULL;
            if (*s == ',') { s = skip_ws(s + 1, e); continue; }
            if (*s == close) return s + 1;
            return NULL;
        }
    }
    default: *t = J_NUM; return skip_num(s, e);
    }
}

int json_parse(const char *s, size_t n, jv *out) {
    const char *e = s + n;
    const char *b = skip_ws(s, e);
    jtype t = J_NONE;
    const char *end = skip_val(b, e, 0, &t);
    if (!end || skip_ws(end, e) != e) { out->p = NULL; out->n = 0; out->t = J_NONE; return -1; }
    out->p = b; out->n = (size_t)(end - b); out->t = t;
    return 0;
}

int json_next(jv c, size_t *pos, jv *key, jv *val) {
    if (c.t != J_OBJ && c.t != J_ARR) return 0;
    const char *e = c.p + c.n, *s = c.p + *pos;
    if (*pos == 0) s = c.p + 1;
    s = skip_ws(s, e);
    if (s < e && *s == ',') s = skip_ws(s + 1, e);
    if (s >= e || *s == '}' || *s == ']') return 0;
    jtype t;
    if (c.t == J_OBJ) {
        const char *k = s;
        s = skip_str(s, e);
        if (!s) return 0;
        if (key) { key->p = k; key->n = (size_t)(s - k); key->t = J_STR; }
        s = skip_ws(s, e);
        if (s >= e || *s != ':') return 0;
        s = skip_ws(s + 1, e);
    } else if (key) { key->p = NULL; key->n = 0; key->t = J_NONE; }
    const char *v = s;
    s = skip_val(s, e, 1, &t);
    if (!s) return 0;
    if (val) { val->p = v; val->n = (size_t)(s - v); val->t = t; }
    *pos = (size_t)(s - c.p);
    return 1;
}

size_t json_len(jv c) {
    size_t pos = 0, n = 0;
    while (json_next(c, &pos, NULL, NULL)) n++;
    return n;
}

static int key_eq(jv k, const char *s, size_t sl) {
    if (k.n < 2) return 0;
    const char *kp = k.p + 1; size_t kn = k.n - 2;
    if (!memchr(kp, '\\', kn)) return kn == sl && memcmp(kp, s, sl) == 0;
    char *tmp = xmalloc(kn + 1);
    long l = json_unescape(kp, kn, tmp);
    int r = l >= 0 && (size_t)l == sl && memcmp(tmp, s, sl) == 0;
    free(tmp);
    return r;
}

int json_get(jv v, const char *path, jv *out) {
    jv cur = v;
    while (path && *path) {
        const char *dot = strchr(path, '.');
        size_t sl = dot ? (size_t)(dot - path) : strlen(path);
        size_t pos = 0; jv k, val; int found = 0;
        if (cur.t == J_OBJ) {
            while (json_next(cur, &pos, &k, &val)) if (key_eq(k, path, sl)) { found = 1; break; }
        } else if (cur.t == J_ARR) {
            size_t idx = 0, want = 0;
            if (sl == 0) return 0;
            for (size_t i = 0; i < sl; i++) {
                if (path[i] < '0' || path[i] > '9') return 0;
                want = want * 10 + (size_t)(path[i] - '0');
            }
            while (json_next(cur, &pos, NULL, &val)) if (idx++ == want) { found = 1; break; }
        }
        if (!found) return 0;
        cur = val;
        path = dot ? dot + 1 : NULL;
    }
    if (out) *out = cur;
    return cur.t != J_NONE;
}

static void put_utf8(char **o, unsigned cp) {
    unsigned char *u = (unsigned char *)*o;
    if (cp < 0x80) *u++ = (unsigned char)cp;
    else if (cp < 0x800) { *u++ = (unsigned char)(0xC0 | (cp >> 6)); *u++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *u++ = (unsigned char)(0xE0 | (cp >> 12)); *u++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        *u++ = (unsigned char)(0x80 | (cp & 0x3F));
    } else {
        *u++ = (unsigned char)(0xF0 | (cp >> 18)); *u++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        *u++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); *u++ = (unsigned char)(0x80 | (cp & 0x3F));
    }
    *o = (char *)u;
}
static long hex4(const char *s) {
    long v = 0;
    for (int i = 0; i < 4; i++) { int h = hexv(s[i]); if (h < 0) return -1; v = v * 16 + h; }
    return v;
}
long json_unescape(const char *in, size_t n, char *out) {
    const char *s = in, *e = in + n; char *o = out;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        if (++s >= e) return -1;
        char c = *s++;
        switch (c) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            if (e - s < 4) return -1;
            long cp = hex4(s);
            if (cp < 0) return -1;
            s += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                long lo = -1;
                if (e - s >= 6 && s[0] == '\\' && s[1] == 'u') lo = hex4(s + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
                else cp = 0xFFFD;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
            put_utf8(&o, (unsigned)cp);
            break;
        }
        default: return -1;
        }
    }
    return (long)(o - out);
}

char *json_str(jv v) {
    if (v.t == J_NONE || !v.p) return NULL;
    if (v.t != J_STR) return xstrndup(v.p, v.n);
    char *o = xmalloc(v.n);
    long l = json_unescape(v.p + 1, v.n - 2, o);
    if (l < 0) l = 0;
    o[l] = 0;
    return o;
}
int json_str_eq(jv v, const char *s) {
    if (v.t != J_STR) return 0;
    jv k = v;
    return key_eq(k, s, strlen(s));
}
int json_streq_path(jv v, const char *path, const char *s) {
    jv x;
    return json_get(v, path, &x) && json_str_eq(x, s);
}
char *json_get_str(jv v, const char *path) {
    jv x;
    if (!json_get(v, path, &x) || x.t == J_NULL) return NULL;
    return json_str(x);
}
double json_num(jv v, double def) {
    if (v.t != J_NUM || v.n >= 64) return def;
    char tmp[64]; memcpy(tmp, v.p, v.n); tmp[v.n] = 0;
    return strtod(tmp, NULL);
}
long long json_int(jv v, long long def) {
    if (v.t != J_NUM) return def;
    return (long long)json_num(v, (double)def);
}
int json_bool(jv v) { return v.t == J_BOOL && v.p[0] == 't'; }
double json_get_num(jv v, const char *path, double def) {
    jv x;
    return json_get(v, path, &x) ? json_num(x, def) : def;
}
void sb_jv_text(sbuf *b, jv v) {
    if (v.t == J_NONE) return;
    if (v.t != J_STR) { sb_add(b, v.p, v.n); return; }
    sb_reserve(b, v.n);
    long l = json_unescape(v.p + 1, v.n - 2, b->p + b->n);
    if (l > 0) b->n += (size_t)l;
    b->p[b->n] = 0;
}
