/* slim-merge: print slim_profile_merge_argv's result, one argument per line
 * (without the engine path), for the benchmark's "cu + profile" configs.
 *   slim-merge LEVEL [ARG...] */
#include "../src/profile.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int level = argc > 1 ? slim_profile_parse(argv[1]) : -1;
    if (level < 0) { fprintf(stderr, "usage: slim-merge off|balanced|max [ARG...]\n"); return 2; }
    char **o = slim_profile_merge_argv(level, "chromium", argc - 2, argv + 2);
    if (!o) return 1;
    for (char **p = o + 1; *p; p++) puts(*p);
    free(o);
    return 0;
}
