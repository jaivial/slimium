/* Shared command context and helpers for src/cmd_*.c */
#ifndef SLIM_CMD_H
#define SLIM_CMD_H
#include "cdp.h"
#include "json.h"
#include "state.h"
#include "util.h"

#define SLIM_VERSION "0.3.1"
#define DEFAULT_TIMEOUT 30000

typedef struct ctx {
    /* global options */
    const char *session;
    int port;          /* --port: talk to any CDP browser (stateless) */
    const char *tab;   /* --tab ID (prefix ok) */
    int json, timeout, timeout_set, in_batch;
    /* lazily established */
    slim_state st; int have_st;
    cdp_t cdp; int connected;
    char *tid, *sid;   /* current target + flatten session */
    int page_on, net_on, rt_on;
    /* emulation keeper: validated pid (0 none), hold_none: none available for this process;
     * bu_accept: accept beforeunload in this session (no keeper); allow_dialog: skip pending check */
    int hold_pid, hold_none, bu_accept, allow_dialog;
    /* output of the current command */
    sbuf oj, ot;
    int usage;
} ctx_t;

typedef int (*cmd_fn)(ctx_t *c, int argc, char **argv);
typedef struct { const char *name; cmd_fn fn; unsigned flags; const char *help; } cmd_def;
#define CF_NOBATCH 1u

/* errors: return -1 from commands after these */
int usage_err(ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* option parsing on a command's argv (in place). Stops at "--". */
const char *opt_val(ctx_t *c, int *argc, char **argv, const char *name);
int opt_flag(int *argc, char **argv, const char *name);
int opt_done(ctx_t *c, int *argc, char **argv); /* rejects leftover --options, removes "--" */
int opt_ms(ctx_t *c, int *argc, char **argv, const char *name, int def);

/* output */
void out_s(ctx_t *c, const char *k, const char *v);
void out_sn(ctx_t *c, const char *k, const char *v, size_t n);
void out_i(ctx_t *c, const char *k, long long v);
void out_d(ctx_t *c, const char *k, double v);
void out_b(ctx_t *c, const char *k, int v);
void out_raw(ctx_t *c, const char *k, const char *json, size_t n);
void out_t(ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* connection */
int ctx_state(ctx_t *c);      /* load session state if in session mode (0 ok) */
int ctx_browser(ctx_t *c);    /* connect browser websocket */
int ctx_page(ctx_t *c);       /* attach to current tab */
void ctx_reset_page(ctx_t *c);
void ctx_disconnect(ctx_t *c);
int ctx_set_target(ctx_t *c, const char *tid); /* remember as current tab */
int ctx_call(ctx_t *c, int page, const char *method, const char *params, char **resp, jv *res);
int ctx_enable(ctx_t *c, const char *domain); /* Page/Network/Runtime once per session */
/* Runtime.evaluate (returnByValue, awaitPromise). *robj = remote object. */
int ctx_eval(ctx_t *c, const char *expr, char **resp, jv *robj);
int ctx_eval_in(ctx_t *c, const char *sid, const char *expr, char **resp, jv *robj);
/* Target list: *arr = targetInfos (view into *resp) */
int ctx_targets(ctx_t *c, char **resp, jv *arr);
int ctx_resolve_target(ctx_t *c, const char *prefix, char **tid); /* id prefix -> full id */
int ctx_wait_load(ctx_t *c, const char *events, int ms); /* wait page event */
int find_browser_pid(int port);
int proc_has_arg(int pid, const char *arg, int *is_child);

/* JS helpers */
void js_el(sbuf *b, const char *sel, const char *body); /* (function(){...e=find(sel)...body})() */
void js_find(sbuf *b, const char *sel, const char *body); /* same, e may be null */
void js_str(sbuf *b, const char *s);
int ctx_attach(ctx_t *c, const char *tid); /* attach + make current tab */
int ctx_state_reload(ctx_t *c);  /* re-read the session state (before changing it) */
/* After changing c->st emulation fields: bump+save the generation, let the keeper apply it (or
 * apply in this session without one). */
int emu_commit(ctx_t *c);
int emu_apply_sync(ctx_t *c, int full);
/* With touch emulation on, click as a touch tap: 0 tapped, 1 not in touch mode, -1 error. */
int ctx_tap(ctx_t *c, double x, double y, int taps);

/* commands */
int cmd_start(ctx_t *, int, char **);
int cmd_stop(ctx_t *, int, char **);
int cmd_status(ctx_t *, int, char **);
int cmd_attach(ctx_t *, int, char **);
int cmd_sessions(ctx_t *, int, char **);
int wrapper_main(int argc, char **argv);

int cmd_tabs(ctx_t *, int, char **);
int cmd_open(ctx_t *, int, char **);
int cmd_close(ctx_t *, int, char **);
int cmd_switch(ctx_t *, int, char **);
int cmd_nav(ctx_t *, int, char **);
int cmd_back(ctx_t *, int, char **);
int cmd_forward(ctx_t *, int, char **);
int cmd_reload(ctx_t *, int, char **);
int cmd_url(ctx_t *, int, char **);
int cmd_title(ctx_t *, int, char **);
int cmd_eval(ctx_t *, int, char **);
int cmd_text(ctx_t *, int, char **);
int cmd_html(ctx_t *, int, char **);
int cmd_shot(ctx_t *, int, char **);
int cmd_pdf(ctx_t *, int, char **);
int cmd_frames(ctx_t *, int, char **);
int cmd_wait(ctx_t *, int, char **);
int cmd_wait_load(ctx_t *, int, char **);
int cmd_wait_idle(ctx_t *, int, char **);
int cmd_freeze(ctx_t *, int, char **);
int cmd_thaw(ctx_t *, int, char **);
int cmd_context_new(ctx_t *, int, char **);
int cmd_context_close(ctx_t *, int, char **);
int cmd_contexts(ctx_t *, int, char **);

int cmd_click(ctx_t *, int, char **);
int cmd_dblclick(ctx_t *, int, char **);
int cmd_hover(ctx_t *, int, char **);
int cmd_type(ctx_t *, int, char **);
int cmd_fill(ctx_t *, int, char **);
int cmd_press(ctx_t *, int, char **);
int cmd_scroll(ctx_t *, int, char **);
int cmd_scroll_to(ctx_t *, int, char **);
int cmd_select(ctx_t *, int, char **);
int cmd_check(ctx_t *, int, char **);

int cmd_cookies(ctx_t *, int, char **);
int cmd_cookie_set(ctx_t *, int, char **);
int cmd_cookies_clear(ctx_t *, int, char **);
int cmd_storage_clear(ctx_t *, int, char **);
int cmd_viewport(ctx_t *, int, char **);
int cmd_ua(ctx_t *, int, char **);
int cmd_offline(ctx_t *, int, char **);
int cmd_block(ctx_t *, int, char **);
int cmd_download_dir(ctx_t *, int, char **);
int cmd_console(ctx_t *, int, char **);

int cmd_mem(ctx_t *, int, char **);
int cmd_purge(ctx_t *, int, char **);
int cmd_cdp(ctx_t *, int, char **);

/* Command modules and in-process invocation (used by MCP and tests). */
const cmd_def *slim_find_module(const char *name);
void slim_help_modules(ctx_t *c);
int cli_invoke(ctx_t *c, int argc, char **argv); /* no stdout emission */

#endif
