/* Locators, stable element refs, snapshots and streaming interaction. */
#ifndef SLIM_INTERACT_H
#define SLIM_INTERACT_H
#include "cmd.h"

/* Called by the shared selector/evaluator compatibility path. */
void interact_js_sel(sbuf *b, const char *sel, const char *body, int must);
int interact_eval_ref(ctx_t *c, const char *expr, char **resp, jv *robj); /* 1 handled, 0 ordinary JS, -1 error */

int interact_snapshot(ctx_t *, int, char **);
int interact_find(ctx_t *, int, char **);
int interact_inspect(ctx_t *, int, char **);
int interact_upload(ctx_t *, int, char **);
int interact_drag(ctx_t *, int, char **);
int interact_focus(ctx_t *, int, char **);
int interact_submit(ctx_t *, int, char **);
int interact_form(ctx_t *, int, char **);
int interact_act(ctx_t *, int, char **);
int interact_pipe(ctx_t *, int, char **);

#endif
