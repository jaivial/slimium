/* Per-session state file: $XDG_RUNTIME_DIR/slimium/<session>.json (fallback /tmp/slimium-$UID). */
#ifndef SLIM_STATE_H
#define SLIM_STATE_H

typedef struct {
    char *name;
    int pid, port, attached, headless;
    char *ws, *target, *profile, *udd, *engine, *log;
    /* emulation re-applied on every attach (CDP resets overrides on disconnect) */
    int vp_w, vp_h, vp_mobile;
    double vp_scale;
    char *ua;
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
