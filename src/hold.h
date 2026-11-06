/* Emulation keeper: one detached process per session that owns emulation and JavaScript dialogs
 * (see docs/EMULATION.md). Commands only notify it; it holds a single browser connection with
 * auto-attach, so overrides live in a persistent DevTools session and survive reloads. */
#ifndef SLIM_HOLD_H
#define SLIM_HOLD_H
#include "cmd.h"

#define EMU_MAX 10
typedef struct { const char *m; char *p; } emu_step;
/* CDP calls that apply the emulation of s to one page session. full: also turn disabled features
 * off; clear: begin with clearDeviceMetricsOverride (re-assert identical metrics); net_on: the page
 * session has Network enabled. base_ua/version: browser default UA and full version (may be NULL).
 * Returns the number of steps (free with emu_plan_free). */
int emu_plan(const slim_state *s, const char *base_ua, const char *version, int full, int clear, int net_on, emu_step *out);
void emu_plan_free(emu_step *st, int n);
int emu_portrait(const slim_state *s);           /* resolved orientation */
int emu_touch(const slim_state *s);              /* resolved touch emulation */

int hold_enabled(void);                          /* SLIM_HOLD != 0 */
int hold_owner(const char *session, int *gen);   /* validated keeper pid (0 none), acked generation */
int hold_spawn(const char *session, int wait_ms);/* make sure a keeper runs: pid or 0 */
/* 0: the keeper applied c->st.gen; 1: no keeper (apply in this session); -1 error */
int hold_ready(ctx_t *c);
/* op "reassert" (re-apply everything) or "dialog" (accept/dismiss the pending dialog of target). */
int hold_request(ctx_t *c, const char *op, const char *target, int accept, const char *text);
int hold_stop(const char *session);              /* 1 stopped, 0 none */
void hold_cleanup(const char *session);          /* remove the keeper's files */
/* manual dialog pending on tid (NULL: any tab): 1 and a one-line description */
int hold_pending(const char *session, const char *tid, char *desc, size_t n);
/* policy text -> "accept" | "dismiss" | "manual" for a dialog type */
const char *dlg_action(const char *policy, const char *type);
int dlg_policy_merge(const char *old, const char *spec, char **out); /* 0 ok */
int dlg_log_count(const char *session, long *size);

#endif
