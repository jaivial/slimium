/* Live Chrome DevTools Protocol schema and target routing helpers. */
#ifndef SLIM_PROTOCOL_H
#define SLIM_PROTOCOL_H
#include "cmd.h"

typedef struct {
    char *body;
    jv root, domains;
} slim_schema;

/* Fetch and parse this browser's /json/protocol. Views remain valid until clear. */
/* Remove insignificant JSON whitespace in place (for one-line --json output). */
size_t slim_json_compact(char *text, size_t len);
int slim_schema_load(ctx_t *c, slim_schema *s);
void slim_schema_clear(slim_schema *s);
jv slim_schema_domain(const slim_schema *s, const char *name);
jv slim_schema_member(jv domain, const char *collection, const char *name);

/* Browser-target heuristic used only when --target is omitted. */
int slim_cdp_browser_domain(const char *method);
/* Select browser, current page, a temporary flattened target attachment, or an
 * existing flattened session. owned_sid is non-NULL only for temporary attachment. */
int slim_cdp_select(ctx_t *c, const char *target, const char *target_id,
                    const char *session_id, int default_page,
                    const char **sid, char **owned_sid);
void slim_cdp_unselect(ctx_t *c, char *owned_sid);

#endif
