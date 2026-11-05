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

/* Memory-relevant switches: the profile's value replaces the caller's (cu passes
 * --renderer-process-limit=4, which would otherwise undo the process model).
 * Every other switch the caller set wins (identity / functional switches:
 * --user-data-dir, --remote-debugging-port, --headless, --user-agent,
 * --window-size, --proxy-server, --lang, ...). */
static const char *const profile_wins[] = {
    "renderer-process-limit", "process-per-site", "process-per-tab", "single-process", "no-zygote",
    "in-process-gpu", "disable-gpu", "disable-gpu-compositing", "disable-software-rasterizer",
    "disable-site-isolation-trials", "enable-low-end-device-mode", "num-raster-threads",
    "force-device-scale-factor", "disk-cache-size", "aggressive-cache-discard", NULL};

/* A profile switch also removes caller switches that contradict it. */
static const struct { const char *flag, *drops[3]; } conflicts[] = {
    {"disable-site-isolation-trials", {"site-per-process", "isolate-origins", NULL}},
    {"process-per-site", {"process-per-tab", NULL, NULL}},
    {"single-process", {"process-per-tab", "process-per-site", NULL}},
};

/* A profile switch is not added when the caller passed any of these: the caller
 * asked for GPU/WebGL, which --disable-gpu / --in-process-gpu would break. */
static const struct { const char *flag, *by[6]; } suppress[] = {
    {"disable-gpu", {"use-gl", "use-angle", "enable-unsafe-swiftshader", "enable-gpu", "ignore-gpu-blocklist", NULL}},
    {"disable-gpu-compositing", {"use-gl", "use-angle", "enable-unsafe-swiftshader", "enable-gpu", NULL, NULL}},
    {"in-process-gpu", {"use-gl", "use-angle", "enable-unsafe-swiftshader", "enable-gpu", NULL, NULL}},
    {"disable-software-rasterizer", {"use-gl", "use-angle", "enable-unsafe-swiftshader", NULL, NULL, NULL}},
};
#define NELEM(a) (sizeof a / sizeof *a)

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

static int same_sw(const char *a, const char *b) {
    size_t n, m; const char *x = sw_name(a, &n), *y = sw_name(b, &m);
    return n == m && !memcmp(x, y, n);
}

static const char *sw_value(const char *a) { const char *e = strchr(a, '='); return e ? e + 1 : ""; }

static int list_of(const char *a) {
    for (size_t i = 0; i < NLISTS; i++) if (sw_is(a, lists[i].name)) return (int)i;
    return -1;
}

static int in_set(const char *a, const char *const *set) {
    for (; *set; set++) if (sw_is(a, *set)) return 1;
    return 0;
}

/* Identity of a list item: feature "Name:param/v" or "Name<Trial" -> "Name";
 * js flag "--no-foo_bar=1" -> "foo-bar" (V8 treats '_' as '-'). */
static void item_key(const char *t, size_t n, int js, char *key, size_t cap) {
    size_t o = 0, i = 0;
    if (js) {
        while (i < n && t[i] == '-') i++;
        if (n - i > 3 && !memcmp(t + i, "no-", 3)) i += 3;
        else if (n - i > 3 && !memcmp(t + i, "no_", 3)) i += 3;
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

/* Should the profile switch f be left out given the caller's argv[0..end)? */
static int profile_skips(const char *f, int end, char **argv) {
    for (size_t s = 0; s < NELEM(suppress); s++)
        if (sw_is(f, suppress[s].flag))
            for (int j = 0; j < end; j++)
                for (size_t b = 0; suppress[s].by[b]; b++)
                    if (is_switch(argv[j]) && sw_is(argv[j], suppress[s].by[b])) return 1;
    if (in_set(f, profile_wins)) return 0;
    for (int j = 0; j < end; j++) if (is_switch(argv[j]) && same_sw(argv[j], f)) return 1;
    return 0;
}

/* Is the caller's switch a overridden by an (emitted) profile switch? */
static int caller_dropped(const char *a, const char *const *f, size_t nf, int end, char **argv) {
    for (size_t i = 0; i < nf; i++) {
        if (list_of(f[i]) >= 0 || profile_skips(f[i], end, argv)) continue;
        if (in_set(f[i], profile_wins) && same_sw(a, f[i])) return 1;
        for (size_t c = 0; c < NELEM(conflicts); c++)
            if (sw_is(f[i], conflicts[c].flag))
                for (size_t d = 0; conflicts[c].drops[d]; d++)
                    if (sw_is(a, conflicts[c].drops[d])) return 1;
    }
    return 0;
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

    /* Merged value of every list switch: the caller's items that the profile
     * does not override (same key in the same list, or the item in the
     * opposite list), then the profile's items. */
    char *merged[NLISTS]; int emitted[NLISTS] = {0};
    for (size_t l = 0; l < NLISTS; l++) {
        int js = lists[l].sep == ' ', caller_has = 0, profile_has = 0;
        const char *veto[16]; size_t nv = 0;
        for (size_t i = 0; i < nf && nv + 1 < NELEM(veto); i++) {
            int fl = list_of(f[i]);
            if (fl == (int)l) profile_has = 1;
            if (fl == (int)l || (lists[l].opp >= 0 && fl == lists[l].opp)) veto[nv++] = sw_value(f[i]);
        }
        veto[nv] = NULL;
        size_t len = 0, nl = strlen(lists[l].name);
        merged[l] = a;
        a[0] = a[1] = '-'; memcpy(a + 2, lists[l].name, nl); a[2 + nl] = '=';
        char *v = a + nl + 3; v[0] = 0;
        for (int i = 0; i < end; i++)
            if (is_switch(argv[i]) && list_of(argv[i]) == (int)l) {
                caller_has = 1; add_items(v, &len, sw_value(argv[i]), lists[l].sep, js, veto);
            }
        for (size_t i = 0; i < nf; i++)
            if (list_of(f[i]) == (int)l) add_items(v, &len, sw_value(f[i]), lists[l].sep, js, NULL);
        if (!len && !(caller_has && !profile_has)) merged[l] = NULL;
        a = v + len + 1;
    }

    size_t k = 0;
    out[k++] = (char *)chromium;
    for (int i = 0; i < end; i++) {
        if (!is_switch(argv[i])) continue;
        int l = list_of(argv[i]);
        if (l >= 0) {
            if (!emitted[l]) { emitted[l] = 1; if (merged[l]) out[k++] = merged[l]; }
        } else if (!caller_dropped(argv[i], f, nf, end, argv)) out[k++] = argv[i];
    }
    for (size_t i = 0; i < nf; i++) {
        int l = list_of(f[i]);
        if (l >= 0) {
            if (!emitted[l]) { emitted[l] = 1; if (merged[l]) out[k++] = merged[l]; }
        } else if (!profile_skips(f[i], end, argv)) {
            size_t j = 0;   /* a profile list may not repeat a switch, but be safe */
            while (j < k && !(is_switch(out[j]) && same_sw(out[j], f[i]))) j++;
            if (j == k) out[k++] = (char *)f[i];
        }
    }
    /* Positional arguments (URLs, about:blank) last, in their order. */
    for (int i = 0; i < argc; i++) if (i >= end || !is_switch(argv[i])) out[k++] = argv[i];
    out[k] = NULL;
    return out;
}
