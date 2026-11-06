/* Per-session state file: $XDG_RUNTIME_DIR/slimium/<session>.json (fallback /tmp/slimium-$UID). */
#ifndef SLIM_STATE_H
#define SLIM_STATE_H

typedef struct {
    char *name;
    int pid, port, attached, headless;
    char *ws, *target, *profile, *user, *udd, *engine, *log;
    /* emulation: owned by the session's keeper ('__hold', see docs/EMULATION.md), re-applied per
     * command only without one. gen is bumped by every emulation/dialog-policy change. */
    int gen;
    int vp_w, vp_h, vp_mobile;
    int vp_orient;    /* 0 auto (portrait iff H >= W), 1 portrait, 2 landscape */
    int vp_touch;     /* 0 auto (= mobile), 1 on, 2 off */
    double vp_scale;
    char *ua;         /* explicit user agent (wins) */
    char *ua_preset;  /* "mobile"|"tablet": Android UA derived from the engine when ua is NULL */
    char *dlg;        /* dialog policy "beforeunload=accept,alert=accept,confirm=dismiss,prompt=dismiss" */
    int offline;
    char *block;  /* JSON array text of URL patterns, NULL = none */
    char *dldir;
} slim_state;

int state_valid_name(const char *name);
int state_dir(char *buf, unsigned n);   /* creates it (0700) */
int state_path(const char *session, const char *ext, char *buf, unsigned n);
int state_load(const char *session, slim_state *st); /* 0 ok, -1 missing/bad (errmsg set) */
int state_save(const slim_state *st);
int state_remove(const char *session);
void state_free(slim_state *st);
void state_set(char **field, const char *val);     /* replace malloc'd string */
/* Is a port used by another saved session? Returns 1 if yes. */
int state_port_taken(int port, const char *except);

#endif
