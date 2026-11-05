/* Unit tests for slim_profile_merge_argv (bench/Makefile: make test). */
#include "../src/profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static int count_prefix(char **v, const char *p) {
    int n = 0; for (; *v; v++) n += !strncmp(*v, p, strlen(p)); return n;
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
/* How many times item is an element of the list value of switch sw
 * (e.g. "--disable-features="). */
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
        char **o = slim_profile_merge_argv(level, chrome, n, cu);
        CHECK(o && !strcmp(o[0], chrome));
        CHECK(!strcmp(o[len(o) - 1], "about:blank"));
        CHECK(count_prefix(o, "--disable-features=") == 1);
        CHECK(count_prefix(o, "--enable-features=") == 1);
        CHECK(count_prefix(o, "--js-flags=") <= 1);
        CHECK(count_prefix(o, "--renderer-process-limit=") == 1);
        CHECK(index_of(o, "--renderer-process-limit=4") > 0);          /* caller wins */
        CHECK(count_prefix(o, "--no-first-run") == 1);                 /* no duplicate */
        CHECK(count_prefix(o, "--disable-gpu") == 1);
        CHECK(count_prefix(o, "--remote-debugging-port=") == 1);
        CHECK(count_prefix(o, "--user-data-dir=") == 1);
        CHECK(count_prefix(o, "--headless") == 1);
        CHECK(index_of(o, cu[10]) > 0);                                /* value with spaces intact */
        CHECK(in_list(o, "--enable-features=", "NetworkServiceInProcess", ','));
        CHECK(in_list(o, "--disable-features=", "Translate", ','));
        CHECK(in_list(o, "--disable-features=", "PaintHolding", ','));
        /* caller's order is kept: port before user-data-dir before headless */
        CHECK(index_of(o, cu[0]) < index_of(o, cu[8]) && index_of(o, cu[8]) < index_of(o, cu[9]));
        if (level == 0) CHECK(len(o) == n + 1);
        size_t np; const char *const *pf = slim_profile_flags(level, &np);
        /* every profile disable-feature lands in the single merged switch */
        for (size_t i = 0; i < np; i++) {
            if (strncmp(pf[i], "--disable-features=", 19)) continue;
            const char *s = pf[i] + 19; char item[128];
            while (*s) {
                size_t m = 0; while (s[m] && s[m] != ',') m++;
                if (m < sizeof item) { memcpy(item, s, m); item[m] = 0; CHECK(in_list(o, "--disable-features=", item, ',')); }
                s += m; while (*s == ',') s++;
            }
        }
        /* no item appears twice in the merged disable list */
        CHECK(in_list(o, "--disable-features=", "Translate", ',') == 1);
        CHECK(in_list(o, "--disable-features=", "BackForwardCache", ',') == 1);
        CHECK(in_list(o, "--disable-features=", "MediaRouter", ',') == 1);
        if (fails) dump(o);
        free(o);
    }

    /* Caller wins on a feature both enabled (caller) and disabled (profile). */
    {
        char *a[] = {"--enable-features=BackForwardCache,Foo:p/1", "https://example.com/"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 2, a);
        CHECK(!in_list(o, "--disable-features=", "BackForwardCache", ','));
        CHECK(in_list(o, "--enable-features=", "BackForwardCache", ','));
        CHECK(in_list(o, "--enable-features=", "Foo:p/1", ','));
        CHECK(in_list(o, "--disable-features=", "Translate", ','));
        CHECK(!strcmp(o[len(o) - 1], "https://example.com/"));
        free(o);
    }
    /* A feature the caller disables is not enabled by the profile. */
    {
        char *a[] = {"--disable-features=NetworkServiceInProcess2"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_MAX, chrome, 1, a);
        CHECK(!in_list(o, "--enable-features=", "NetworkServiceInProcess2", ','));
        CHECK(in_list(o, "--disable-features=", "NetworkServiceInProcess2", ','));
        free(o);
    }
    /* Two caller occurrences (e.g. a distro wrapper's and cu's) collapse into one. */
    {
        char *a[] = {"--disable-features=A,B", "--x", "--disable-features=B,C", "about:blank"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_OFF, chrome, 4, a);
        CHECK(count_prefix(o, "--disable-features=") == 1);
        CHECK(!strcmp(find(o, "--disable-features="), "--disable-features=A,B,C"));
        CHECK(index_of(o, "--disable-features=A,B,C") == 1 && index_of(o, "--x") == 2);
        free(o);
    }
    /* js-flags: space list, caller's value wins per V8 flag (incl. --no- and _). */
    {
        char *a[] = {"--js-flags=--max_lazy=0 --expose-gc"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_MAX, chrome, 1, a);
        const char *j = find(o, "--js-flags=");
        CHECK(j && strstr(j, "--expose-gc") && strstr(j, "--max_lazy=0"));
        CHECK(j && !strstr(j, "--max-lazy"));
        CHECK(count_prefix(o, "--js-flags=") == 1);
        free(o);
        char *b[] = {"--js-flags=--no-optimize-for-size"};
        o = slim_profile_merge_argv(SLIM_PROFILE_MAX, chrome, 1, b);
        j = find(o, "--js-flags=");
        CHECK(j && !strstr(j, " --optimize-for-size") && strstr(j, "--no-optimize-for-size"));
        free(o);
    }
    /* No caller args: just chromium + the profile, lists present once. */
    {
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 0, NULL);
        size_t np; slim_profile_flags(SLIM_PROFILE_BALANCED, &np);
        CHECK((size_t)len(o) == np + 1);
        CHECK(count_prefix(o, "--disable-features=") == 1);
        free(o);
    }
    /* GL requested by the caller: --disable-gpu is not forced on it. */
    {
        char *a[] = {"--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_MAX, chrome, 3, a);
        CHECK(index_of(o, "--disable-gpu") < 0 && index_of(o, "--in-process-gpu") < 0);
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
    /* Single-dash switches count as set by the caller. */
    {
        char *a[] = {"-disable-gpu", "-renderer-process-limit=2"};
        char **o = slim_profile_merge_argv(SLIM_PROFILE_BALANCED, chrome, 2, a);
        CHECK(index_of(o, "--disable-gpu") < 0 && count_prefix(o, "--renderer-process-limit") == 0);
        free(o);
    }
    /* Parse. */
    CHECK(slim_profile_parse("off") == 0 && slim_profile_parse("balanced") == 1 && slim_profile_parse("max") == 2);
    CHECK(slim_profile_parse("2") == 2 && slim_profile_parse("x") == -1 && slim_profile_parse(NULL) == -1);

    printf("test_profile: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
