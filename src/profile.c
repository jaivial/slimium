/* Low-memory Chromium flag profiles and the switch-aware argv merge.
 * The flag lists are tuned empirically: see docs/PROFILE.md and bench/. */
#include "profile.h"
#include <stdlib.h>
#include <string.h>

static const char *const balanced[] = {
    "--renderer-process-limit=1",
    "--process-per-site",
    "--disable-site-isolation-trials",
    "--disable-features=IsolateOrigins,site-per-process,Translate,OptimizationHints,MediaRouter,"
    "BackForwardCache,InterestFeedContentSuggestions,CalculateNativeWinOcclusion,AutofillServerCommunication",
    "--enable-low-end-device-mode",
    "--disable-gpu",
    "--disable-extensions",
    "--disable-component-extensions-with-background-pages",
    "--disable-background-networking",
    "--disable-default-apps",
    "--disable-sync",
    "--no-first-run",
    "--disable-breakpad",
    "--js-flags=--max-lazy",
    NULL};

static const char *const max_[] = {
    "--renderer-process-limit=1",
    "--process-per-site",
    "--disable-site-isolation-trials",
    "--disable-features=IsolateOrigins,site-per-process,Translate,OptimizationHints,MediaRouter,"
    "BackForwardCache,InterestFeedContentSuggestions,CalculateNativeWinOcclusion,AutofillServerCommunication",
    "--enable-features=NetworkServiceInProcess2",
    "--in-process-gpu",
    "--enable-low-end-device-mode",
    "--disable-gpu",
    "--disable-extensions",
    "--disable-component-extensions-with-background-pages",
    "--disable-background-networking",
    "--disable-default-apps",
    "--disable-sync",
    "--no-first-run",
    "--disable-breakpad",
    "--js-flags=--max-lazy --optimize-for-size",
    NULL};

int slim_profile_parse(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "off") || !strcmp(s, "0")) return SLIM_PROFILE_OFF;
    if (!strcmp(s, "balanced") || !strcmp(s, "1")) return SLIM_PROFILE_BALANCED;
    if (!strcmp(s, "max") || !strcmp(s, "2")) return SLIM_PROFILE_MAX;
    return -1;
}

const char *const *slim_profile_flags(int level, size_t *count) {
    static const char *const none[] = {NULL};
    const char *const *l = level == SLIM_PROFILE_MAX ? max_ : level == SLIM_PROFILE_BALANCED ? balanced : none;
    if (count) { size_t n = 0; while (l[n]) n++; *count = n; }
    return l;
}

/* ---- merge ---------------------------------------------------------------- */

/* List-valued switches: every occurrence is merged into one, because Chromium
 * keeps only the last value of a switch. opp = index of the opposite list
 * (enable <-> disable) or -1. */
static const struct { const char *name; char sep; int opp; } lists[] = {
    {"enable-features", ',', 1},       {"disable-features", ',', 0},
    {"enable-blink-features", ',', 3}, {"disable-blink-features", ',', 2},
    {"js-flags", ' ', -1},
};
#define NLISTS (sizeof lists / sizeof *lists)

/* A profile switch is not added when the caller passed any of these: the caller
 * asked for GPU/WebGL, which --disable-gpu / --in-process-gpu would break. */
static const struct { const char *flag, *by[6]; } suppress[] = {
    {"disable-gpu", {"use-gl", "use-angle", "enable-unsafe-swiftshader", "enable-gpu", "ignore-gpu-blocklist", NULL}},
    {"in-process-gpu", {"use-gl", "use-angle", "enable-unsafe-swiftshader", "enable-gpu", "disable-gpu-process-crash-limit", NULL}},
    {"disable-software-rasterizer", {"use-gl", "use-angle", "enable-unsafe-swiftshader", NULL, NULL, NULL}},
};

static int is_switch(const char *a) { return a[0] == '-' && a[1] && strcmp(a, "--"); }

/* Name of a switch without dashes and value; *len gets its length. */
static const char *sw_name(const char *a, size_t *len) {
    a += a[1] == '-' ? 2 : 1;
    const char *e = strchr(a, '=');
    *len = e ? (size_t)(e - a) : strlen(a);
    return a;
}

static int sw_is(const char *a, const char *name) {
    size_t n; const char *s = sw_name(a, &n);
    return n == strlen(name) && !memcmp(s, name, n);
}

static const char *sw_value(const char *a) { const char *e = strchr(a, '='); return e ? e + 1 : ""; }

static int list_of(const char *a) {
    for (size_t i = 0; i < NLISTS; i++) if (sw_is(a, lists[i].name)) return (int)i;
    return -1;
}

/* Identity of a list item: feature "Name:param/v" or "Name<Trial" -> "Name";
 * js flag "--no-foo_bar=1" -> "foo-bar" (V8 treats '_' as '-'). */
static void item_key(const char *t, size_t n, int js, char *key, size_t cap) {
    size_t o = 0, i = 0;
    if (js) {
        while (i < n && t[i] == '-') i++;
        if (n - i > 3 && !memcmp(t + i, "no-", 3)) i += 3;
        else if (n - i > 2 && !memcmp(t + i, "no", 2) && t[i + 2] == '_') i += 3;
        for (; i < n && t[i] != '=' && o + 1 < cap; i++) key[o++] = t[i] == '_' ? '-' : t[i];
    } else {
        if (i < n && t[i] == '*') i++;
        for (; i < n && t[i] != ':' && t[i] != '<' && o + 1 < cap; i++) key[o++] = t[i];
    }
    key[o] = 0;
}

/* Does the value list v (separator sep) contain an item with this key? */
static int has_key(const char *v, char sep, int js, const char *key) {
    char k[256];
    while (*v) {
        while (*v == sep) v++;
        size_t n = 0; while (v[n] && v[n] != sep) n++;
        if (n) { item_key(v, n, js, k, sizeof k); if (!strcmp(k, key)) return 1; }
        v += n;
    }
    return 0;
}

/* Append the items of src to the list being built at dst (current length
 * *len), skipping items whose key is already in dst or in any string of
 * the NULL-terminated veto array. */
static void add_items(char *dst, size_t *len, const char *src, char sep, int js, const char *const *veto) {
    char k[256];
    while (*src) {
        while (*src == sep) src++;
        size_t n = 0; while (src[n] && src[n] != sep) n++;
        if (!n) break;
        item_key(src, n, js, k, sizeof k);
        dst[*len] = 0;
        int skip = !k[0] || has_key(dst, sep, js, k);
        for (size_t i = 0; !skip && veto && veto[i]; i++) skip = has_key(veto[i], sep, js, k);
        if (!skip) {
            if (*len) dst[(*len)++] = sep;
            memcpy(dst + *len, src, n); *len += n; dst[*len] = 0;
        }
        src += n;
    }
}

char **slim_profile_merge_argv(int level, const char *chromium, int argc, char **argv) {
    size_t nf; const char *const *f = slim_profile_flags(level, &nf);
    /* One allocation: the pointer array, then an arena for the merged lists
     * (each merged value is at most every contributing string plus its name),
     * so free(result) releases everything. */
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 2;
    for (size_t i = 0; i < nf; i++) total += strlen(f[i]) + 2;
    size_t nptr = (size_t)argc + nf + 2, arena = NLISTS * (total + 32);
    char **out = malloc(nptr * sizeof *out + arena);
    if (!out) return NULL;
    char *a = (char *)(out + nptr);

    int end = argc;     /* first positional index after a bare "--" */
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--")) { end = i; break; }

    /* Merged value of every list switch: caller items first (caller wins),
     * then profile items not set or opposed by the caller. */
    char *merged[NLISTS]; int caller_has[NLISTS] = {0}, emitted[NLISTS] = {0};
    for (size_t l = 0; l < NLISTS; l++) {
        int js = lists[l].sep == ' ';
        size_t len = 0; merged[l] = a; a[0] = 0;
        size_t nl = strlen(lists[l].name);
        a[0] = a[1] = '-'; memcpy(a + 2, lists[l].name, nl); a[2 + nl] = '=';
        char *v = a + nl + 3; v[0] = 0;
        for (int i = 0; i < end; i++)
            if (is_switch(argv[i]) && list_of(argv[i]) == (int)l) {
                caller_has[l] = 1; add_items(v, &len, sw_value(argv[i]), lists[l].sep, js, NULL);
            }
        /* Items the caller put in the opposite list veto the profile's. */
        const char *veto[64]; size_t nv = 0;
        if (lists[l].opp >= 0)
            for (int i = 0; i < end && nv + 1 < 64; i++)
                if (is_switch(argv[i]) && list_of(argv[i]) == lists[l].opp) veto[nv++] = sw_value(argv[i]);
        veto[nv] = NULL;
        int profile_has = 0;
        for (size_t i = 0; i < nf; i++)
            if (list_of(f[i]) == (int)l) { profile_has = 1; add_items(v, &len, sw_value(f[i]), lists[l].sep, js, veto); }
        if (!caller_has[l] && (!profile_has || !len)) merged[l] = NULL;
        a = v + len + 1;
    }

    size_t k = 0;
    out[k++] = (char *)chromium;
    for (int i = 0; i < end; i++) {
        if (!is_switch(argv[i])) continue;
        int l = list_of(argv[i]);
        if (l < 0) out[k++] = argv[i];
        else if (!emitted[l]) { emitted[l] = 1; out[k++] = merged[l]; }
    }
    for (size_t i = 0; i < nf; i++) {
        int l = list_of(f[i]);
        if (l >= 0) {
            if (merged[l] && !emitted[l]) { emitted[l] = 1; out[k++] = merged[l]; }
            continue;
        }
        size_t n; const char *name = sw_name(f[i], &n);
        int skip = 0;
        for (int j = 0; j < end && !skip; j++) {
            size_t m; const char *s;
            if (!is_switch(argv[j])) continue;
            s = sw_name(argv[j], &m);
            skip = m == n && !memcmp(s, name, n);
        }
        for (size_t s = 0; !skip && s < sizeof suppress / sizeof *suppress; s++)
            if (sw_is(f[i], suppress[s].flag))
                for (int j = 0; j < end && !skip; j++)
                    for (size_t b = 0; !skip && suppress[s].by[b]; b++)
                        skip = is_switch(argv[j]) && sw_is(argv[j], suppress[s].by[b]);
        if (!skip) out[k++] = (char *)f[i];
    }
    /* Positional arguments (URLs, about:blank) last, in their order. */
    for (int i = 0; i < argc; i++) if (i >= end || !is_switch(argv[i])) out[k++] = argv[i];
    out[k] = NULL;
    return out;
}
