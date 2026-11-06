/* Chrome DevTools Protocol client over one browser-level websocket (flatten sessions). */
#ifndef SLIM_CDP_H
#define SLIM_CDP_H
#include "json.h"
#include "ws.h"

typedef struct cdp_ev { struct cdp_ev *next; char *msg; size_t len; } cdp_ev;

typedef struct {
    ws_t w;
    int open, debug;   /* debug: SLIM_DEBUG=1 traces traffic to stderr */
    long next_id;
    cdp_ev *head, *tail;
    size_t nev, evbytes;
    /* optional: called about every 100 ms while cdp_wait/cdp_event block; non-zero aborts the
     * wait with the hook's error (used to fail fast on JavaScript dialogs) */
    int (*hook)(void *ud);
    void *hook_ud;
    long long hook_next;
} cdp_t;

/* GET http://127.0.0.1:port/json/version -> webSocketDebuggerUrl (malloc'd). */
int cdp_browser_url(int port, int timeout_ms, char **url, char **product);
int cdp_connect(cdp_t *c, const char *ws_url, int timeout_ms);
void cdp_close(cdp_t *c);

/* Send a command (params: JSON object text or NULL). sid NULL = browser target. */
int cdp_send(cdp_t *c, const char *sid, const char *method, const char *params, long *id);
/* Wait for the response to id. On success *resp (malloc'd, may be NULL to discard) and *result
 * (view into *resp). Returns 0 ok, -1 error (CDP error message / timeout / detached). */
int cdp_wait(cdp_t *c, long id, const char *sid, int timeout_ms, char **resp, jv *result);
int cdp_call(cdp_t *c, const char *sid, const char *method, const char *params, int timeout_ms, char **resp, jv *result);
/* Wait for an event. methods: "A" or "A|B|C"; sid NULL matches browser-level events only,
 * sid "*" matches any. 1 got (*msg malloc'd, *params view, *method_out view), 0 timeout, -1 error. */
int cdp_event(cdp_t *c, const char *sid, const char *methods, int timeout_ms, char **msg, jv *params, jv *method_out);
/* Next raw message (queued first): 1 got (*msg malloc'd), 0 timeout, -1 error. */
int cdp_next(cdp_t *c, int timeout_ms, char **msg, size_t *len);
/* Drop buffered events matching (methods NULL = all for that sid). */
void cdp_flush(cdp_t *c, const char *sid, const char *methods);

/* Target.attachToTarget flatten:true -> sessionId (malloc'd). */
int cdp_attach(cdp_t *c, const char *target_id, int timeout_ms, char **sid);
void cdp_detach(cdp_t *c, const char *sid, int timeout_ms);

#endif
