/* Unit tests for slim_profile_merge_argv (bench/Makefile: make test).
 * Profile-dependent expectations are derived from slim_profile_flags(), so the
 * tests keep holding while the flag lists are tuned. */
#include "../src/profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static int count_prefix(char **v, const char *p) {
    int n = 0;
    for (; *v; v++) n += !strncmp(*v, p, strlen(p));
    return n;
}
static const char *find(char **v, const char *p) {
    for (; *v; v++) if (!strncmp(*v, p, strlen(p))) return *v;
    return NULL;
}
static int index_of(char **v, const char *s) {
    for (int i = 0; v[i]; i++) if (!strcmp(v[i], s)) return i;
    return -1;
}
static int len(char **v) { int n = 0; while (v[n]) n++; return n; }
/* How many times item is an element of the list value of switch sw. */
static int in_list(char **v, const char *sw, const char *item, char sep) {
    const char *s = find(v, sw); int c = 0;
    if (!s) return 0;
    s += strlen(sw); size_t n = strlen(item);
    while (*s) {
        size_t m = 0; while (s[m] && s[m] != sep) m++;
        if (m == n && !memcmp(s, item, n)) c++;
        s += m; while (*s == sep) s++;
    }
    return c;
}
/* First item of the profile's list switch sw (e.g. "--disable-features="). */
static int profile_item(int level, const char *sw, char sep, char *out, size_t cap) {
    size_t n; const char *const *f = slim_profile_flags(level, &n);
    for (size_t i = 0; i < n; i++)
        if (!strncmp(f[i], sw, strlen(sw))) {
            const char *s = f[i] + strlen(sw); size_t m = 0;
            while (s[m] && s[m] != sep && m + 1 < cap) { out[m] = s[m]; m++; }
            out[m] = 0; return m > 0;
        }
    return 0;
}
static const char *profile_flag(int level, const char *prefix) {
    size_t n; const char *const *f = slim_profile_flags(level, &n);
    for (size_t i = 0; i < n; i++) if (!strncmp(f[i], prefix, strlen(prefix))) return f[i];
    return NULL;
}

#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void dump(char **v) { for (; *v; v++) fprintf(stderr, "  [%s]\n", *v); }

int main(void) {
    const char *chrome = "/usr/lib64/chromium-browser/chromium-browser";

    /* cu FastTest argv, as cu passes it (policy.rs). */
    char *cu[] = {"--remote-debugging-port=9222", "--no-first-run", "--disable-dev-shm-usage",
                  "--disable-gpu", "--enable-features=NetworkServiceInProcess",
                  "--disable-site-isolation-trials", "--renderer-process-limit=4",
                  "--disable-features=Translate,OptimizationHints,MediaRouter,BackForwardCache,PaintHolding",
                  "--user-data-dir=/tmp/x", "--headless=new",
                  "--user-agent=Mozilla/5.0 (X11; Linux x86_64) Chrome/156.0.0.0 Safari/537.36",
                  "about:blank"};
    int n = (int)(sizeof cu / sizeof *cu);
    for (int level = 0; level <= 2; level++) {
        int f0 = fails;
        char **o = slim_profile_merge_argv(level, chrome, n, cu);
        CHECK(o && !strcmp(o[0], chrome));
        CHECK(!strcmp(o[len(o) - 1], "about:blank"));
        CHECK(count_prefix(o, "--disable-features=") == 1);
        CHECK(count_prefix(o, "--enable-features=") == 1);
        CHECK(count_prefix(o, "--js-flags=") <= 1);
        CHECK(count_prefix(o, "--renderer-process-limit=") == 1);
        /* the profile's process limit overrides cu's 4 (memory switch) */
        const char *rpl = profile_flag(level, "--renderer-process-limit=");
        CHECK(index_of(o, rpl ? rpl : "--renderer-process-limit=4") > 0);
        CHECK(count_prefix(o, "--no-first-run") == 1);                 /* no duplicate */
        CHECK(count_prefix(o, "--disable-gpu") <= 1);
        CHECK(count_prefix(o, "--disable-site-isolation-trials") == 1);
        CHECK(count_prefix(o, "--remote-debugging-port=") == 1);        /* identity: caller's */
        CHECK(index_of(o, "--remote-debugging-port=9222") > 0);
        CHECK(index_of(o, "--user-data-dir=/tmp/x") > 0);
        CHECK(count_prefix(o, "--headless") == 1);
        CHECK(index_of(o, cu[10]) > 0);                                /* value with spaces intact */
        CHECK(in_list(o, "--enable-features=", "NetworkServiceInProcess", ','));
        CHECK(in_list(o, "--disable-features=", "Translate", ',') == 1);
        CHECK(in_list(o, "--disable-features=", "PaintHolding", ',') == 1);
        CHECK(in_list(o, "--disable-features=", "BackForwardCache", ',') == 1);
        CHECK(index_of(o, cu[0]) < index_of(o, cu[8]) && index_of(o, cu[8]) < index_of(o, cu[9]));
        if (level == 0) CHECK(len(o) == n + 1);
        size_t np; const char *const *pf = slim_profile_flags(level, &np);
        /* every profile feature lands, once, in the single merged switch */
        for (size_t i = 0; i < np; i++) {
            const char *sw = !strncmp(pf[i], "--disable-features=", 19) ? "--disable-features="
                           : !strncmp(pf[i], "--enable-features=", 18) ? "--enable-features=" : NULL;
            if (!sw) continue;
            const char *s = pf[i] + strlen(sw); char item[128];
            while (*s) {
                size_t m = 0; while (s[m] && s[m] != ',') m++;
                if (m < sizeof item) { memcpy(item, s, m); item[m] = 0; CHECK(in_list(o, sw, item, ',') == 1); }
                s += m; while (*s == ',') s++;
            }
        }
        /* every profile scalar switch is present exactly once */
        for (size_t i = 0; i < np; i++) {
            if (!strncmp(pf[i], "--disable-features=", 19) || !strncmp(pf[i], "--enable-features=", 18) ||
                !strncmp(pf[i], "--js-flags=", 11)) continue;
            const char *eq = strchr(pf[i], '=');
            char name[128]; size_t m = eq ? (size_t)(eq - pf[i]) : strlen(pf[i]);
            if (m >= sizeof name) continue;
            memcpy(name, pf[i], m); name[m] = 0;
            int c = 0; for (char **v = o; *v; v++) c += !strncmp(*v, name, m) && ((*v)[m] == '=' || !(*v)[m]);
            CHECK(c == 1);
        }
        if (fails != f0) dump(o);
        free(o);
    }

    for (int level = 1; level <= 2; level++) {
        char item[128], arg[256];
        /* Profile wins on a memory feature the caller enabled. */
        if (profile_item(level, "--disable-features=", ',', item, sizeof item)) {
            snprintf(arg, sizeof arg, "--enable-features=%s,Foo:p/1", item);
            char *a[] = {arg, "https://example.com/"};
            char **o = slim_profile_merge_argv(level, chrome, 2, a);
            CHECK(in_list(o, "--disable-features=", item, ',') == 1);
            CHECK(!in_list(o, "--enable-features=", item, ','));
            CHECK(in_list(o, "--enable-features=", "Foo:p/1", ','));
            CHECK(!strcmp(o[len(o) - 1], "https://example.com/"));
            free(o);
        }
        /* ... and on a feature it enables that the caller disabled. */
        if (profile_item(level, "--enable-features=", ',', item, sizeof item)) {
            snprintf(arg, sizeof arg, "--disable-features=%s,Bar", item);
            char *a[] = {arg};
            char **o = slim_profile_merge_argv(level, chrome, 1, a);
            CHECK(!in_list(o, "--disable-features=", item, ','));
            CHECK(in_list(o, "--disable-features=", "Bar", ','));
            CHECK(in_list(o, "--enable-features=", item, ',') == 1);
            free(o);
        }
        /* js-flags: one switch; the profile's V8 options replace the caller's
         * same-named ones (incl. --no- and '_' spellings); others are kept. */
        if (profile_item(level, "--js-flags=", ' ', item, sizeof item)) {
            char key[128]; const char *k = item; while (*k == '-') k++;
            size_t kl = strcspn(k, "="); if (kl >= sizeof key) kl = sizeof key - 1;
            memcpy(key, k, kl); key[kl] = 0;
            for (char *p = key; *p; p++) if (*p == '-') *p = '_';
            snprintf(arg, sizeof arg, "--js-flags=--no_%s --expose-gc", key);
            char *a[] = {arg};
            char **o = slim_profile_merge_argv(level, chrome, 1, a);
            const char *j = find(o, "--js-flags=");
            CHECK(count_prefix(o, "--js-flags=") == 1);
            CHECK(j && strstr(j, "--expose-gc") && strstr(j, item) && !strstr(j, "--no_"));
            free(o);
        }
    }
    /* A list switch the caller passed twice (distro wrapper + cu) collapses into one. */
    {
        char *a[] = {"--disable-features=A,B", "--x", "--disable-features=B,C", "about:blank"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_OFF, chrome, 4, a);
        CHECK(count_prefix(o, "--disable-features=") == 1);
        CHECK(!strcmp(find(o, "--disable-features="), "--disable-features=A,B,C"));
        CHECK(index_of(o, "--disable-features=A,B,C") == 1 && index_of(o, "--x") == 2);
        free(o);
    }
    /* Caller js-flags kept as-is with profile off. */
    {
        char *a[] = {"--js-flags=--max_lazy=0 --expose-gc"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_OFF, chrome, 1, a);
        CHECK(!strcmp(find(o, "--js-flags="), "--js-flags=--max_lazy=0 --expose-gc"));
        free(o);
    }
    /* No caller args: just chromium + the profile, lists present once. */
    {
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 0, NULL);
        size_t np; slim_profile_flags(SLIM_PROFILE_BALANCED, &np);
        CHECK((size_t)len(o) == np + 1);
        CHECK(count_prefix(o, "--disable-features=") <= 1);
        free(o);
    }
    /* GL requested by the caller: no GPU-off switch is forced on it. */
    {
        char *a[] = {"--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"};
        for (int level = 0; level <= 2; level++) {
            char **o = slim_profile_merge_argv(level, chrome, 3, a);
            CHECK(index_of(o, "--disable-gpu") < 0 && index_of(o, "--in-process-gpu") < 0);
            CHECK(index_of(o, "--disable-software-rasterizer") < 0);
            free(o);
        }
    }
    /* Identity switches stay the caller's. */
    {
        char *a[] = {"--window-size=800,600", "--proxy-server=http://p:1", "--lang=es", "--headless=old"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_MAX, chrome, 4, a);
        for (int i = 0; i < 4; i++) CHECK(index_of(o, a[i]) > 0);
        CHECK(count_prefix(o, "--window-size") == 1 && count_prefix(o, "--headless") == 1);
        free(o);
    }
    /* Contradicting caller switches go when the profile sets the process model. */
    if (profile_flag(SLIM_PROFILE_BALANCED, "--disable-site-isolation-trials")) {
        char *a[] = {"--site-per-process", "--isolate-origins=https://a.test", "--keep-me"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 3, a);
        CHECK(index_of(o, "--site-per-process") < 0 && count_prefix(o, "--isolate-origins") == 0);
        CHECK(index_of(o, "--keep-me") > 0);
        free(o);
    }
    /* Positional args keep their order; everything after "--" is positional. */
    {
        char *a[] = {"https://a.test/", "--incognito", "https://b.test/", "--", "--not-a-switch"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 5, a);
        int m = len(o);
        CHECK(!strcmp(o[m - 4], "https://a.test/") && !strcmp(o[m - 3], "https://b.test/"));
        CHECK(!strcmp(o[m - 2], "--") && !strcmp(o[m - 1], "--not-a-switch"));
        CHECK(index_of(o, "--incognito") > 0 && index_of(o, "--incognito") < m - 4);
        free(o);
    }
    /* Single-dash switches are recognised by name. */
    {
        char *a[] = {"-no-first-run", "-renderer-process-limit=2"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 2, a);
        CHECK(count_prefix(o, "--no-first-run") + count_prefix(o, "-no-first-run") == 1);
        CHECK(count_prefix(o, "--renderer-process-limit") + count_prefix(o, "-renderer-process-limit") == 1);
        free(o);
    }
    /* Parse. */
    CHECK(slim_profile_parse("off") == 0 && slim_profile_parse("balanced") == 1 && slim_profile_parse("max") == 2);
    CHECK(slim_profile_parse("2") == 2 && slim_profile_parse("x") == -1 && slim_profile_parse(NULL) == -1);

    printf("test_profile: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
