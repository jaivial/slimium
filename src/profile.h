#ifndef SLIM_PROFILE_H
#define SLIM_PROFILE_H
#include <stddef.h>

/* Memory profile levels. */
enum { SLIM_PROFILE_OFF = 0, SLIM_PROFILE_BALANCED = 1, SLIM_PROFILE_MAX = 2 };

/* Parse "off" | "balanced" | "max" (or "0".."2"); returns -1 on error. */
int slim_profile_parse(const char *s);

/* NULL-terminated list of Chromium switches for a level. Static storage. */
const char *const *slim_profile_flags(int level, size_t *count);

/* Merge the profile into a caller's argv (argv excludes the program name).
 * Result: chromium, the caller's switches (in order), the profile switches the
 * caller did not set, then the caller's positional args (URLs, about:blank,
 * everything after a bare "--") in order.
 * - Scalar switches: the caller wins (a profile switch whose name the caller
 *   passed is dropped). --disable-gpu/--in-process-gpu are also dropped when the
 *   caller asks for GL (--use-gl, --use-angle, --enable-unsafe-swiftshader...).
 * - --enable-features, --disable-features, --enable-blink-features,
 *   --disable-blink-features (comma lists) and --js-flags (space list) end up as
 *   ONE switch each (Chromium honours only the last occurrence), holding every
 *   caller item (all occurrences) plus profile items the caller did not name; a
 *   profile item the caller put in the opposite list is dropped (caller wins).
 * Returns ONE malloc'd block (pointer array + merged strings): free() it once.
 * The other strings point into argv / static storage. NULL on OOM. */
char **slim_profile_merge_argv(int level, const char *chromium, int argc, char **argv);

#endif