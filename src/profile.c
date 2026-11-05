/* Initial low-memory profile. The memory track tunes this empirically (see bench/). */
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

/* Placeholder merge: chromium, caller args, then profile flags. The memory track
 * replaces this with a real switch-aware merge. */
char **slim_profile_merge_argv(int level, const char *chromium, int argc, char **argv) {
    size_t n; const char *const *f = slim_profile_flags(level, &n);
    char **out = calloc((size_t)argc + n + 2, sizeof *out);
    if (!out) return NULL;
    size_t k = 0; out[k++] = (char *)chromium;
    for (int i = 0; i < argc; i++) out[k++] = argv[i];
    for (size_t i = 0; i < n; i++) out[k++] = (char *)f[i];
    out[k] = NULL; return out;
}
