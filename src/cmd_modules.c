/* Optional command modules. Weak references keep partial builds linkable. */
#include "cmd.h"
#include <string.h>

typedef const cmd_def *(*module_fn)(size_t *count);
extern const cmd_def *slim_user_profile_commands(size_t *) __attribute__((weak));
extern const cmd_def *slim_interact_commands(size_t *) __attribute__((weak));
extern const cmd_def *slim_media_commands(size_t *) __attribute__((weak));
extern const cmd_def *slim_protocol_commands(size_t *) __attribute__((weak));
extern const cmd_def *slim_mcp_commands(size_t *) __attribute__((weak));

static const module_fn modules[] = {
    slim_user_profile_commands,
    slim_interact_commands,
    slim_media_commands,
    slim_protocol_commands,
    slim_mcp_commands,
};

const cmd_def *slim_find_module(const char *name) {
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        if (!modules[i]) continue;
        size_t n = 0;
        const cmd_def *defs = modules[i](&n);
        for (size_t k = 0; defs && k < n; k++)
            if (!strcmp(defs[k].name, name)) return &defs[k];
    }
    return NULL;
}

void slim_help_modules(ctx_t *c) {
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        if (!modules[i]) continue;
        size_t n = 0;
        const cmd_def *defs = modules[i](&n);
        for (size_t k = 0; defs && k < n; k++)
            out_t(c, "  %-14s %s\n", defs[k].name, defs[k].help);
    }
}
