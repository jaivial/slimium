#ifndef SLIM_PROFILE_H
#define SLIM_PROFILE_H
#include <stddef.h>

/* Memory profile levels. */
enum { SLIM_PROFILE_OFF = 0, SLIM_PROFILE_BALANCED = 1, SLIM_PROFILE_MAX = 2 };

/* Parse "off" | "balanced" | "max" (or "0".."2"); returns -1 on error. */
int slim_profile_parse(const char *s);

/* NULL-terminated list of Chromium switches for a level. Static storage. */
const char *const *slim_profile_flags(int level, size_t *count);

/* Merge: append profile flags to argv (chromium path first), dropping profile
 * flags the caller already set (by switch name) and merging --disable-features /
 * --enable-features / --js-flags values. Returns malloc'd NULL-terminated argv. */
char **slim_profile_merge_argv(int level, const char *chromium, int argc, char **argv);

#endif
