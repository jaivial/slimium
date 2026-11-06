#include "state.h"
#include "json.h"
#include "util.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int state_valid_name(const char *n) {
    if (!n || !*n || *n == '.' || strlen(n) > 64) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return 0;
    return 1;
}

int state_dir(char *buf, unsigned n) {
    const char *x = getenv("SLIM_STATE_DIR");
    int k;
    if (x && *x) k = snprintf(buf, n, "%s", x);
    else {
        x = getenv("XDG_RUNTIME_DIR");
        struct stat st;
        if (x && *x == '/' && stat(x, &st) == 0 && S_ISDIR(st.st_mode) && access(x, W_OK) == 0)
            k = snprintf(buf, n, "%s/slimium", x);
        else k = snprintf(buf, n, "/tmp/slimium-%u", (unsigned)getuid());
    }
    if (k < 0 || (unsigned)k >= n) return slim_seterr("state dir path too long");
    return mkdir_p(buf, 0700);
}

int state_path(const char *session, const char *ext, char *buf, unsigned n) {
    if (!state_valid_name(session)) return slim_seterr("invalid session name '%s'", session ? session : "");
    char d[3072];
    if (state_dir(d, sizeof d)) return -1;
    int k = snprintf(buf, n, "%s/%s%s", d, session, ext);
    if (k < 0 || (unsigned)k >= n) return slim_seterr("state path too long");
    return 0;
}

void state_set(char **f, const char *v) { free(*f); *f = v ? xstrdup(v) : NULL; }

void state_free(slim_state *s) {
    free(s->name); free(s->ws); free(s->target); free(s->profile); free(s->user); free(s->udd);
    free(s->engine); free(s->log); free(s->ua); free(s->block); free(s->dldir);
    memset(s, 0, sizeof *s);
}

int state_load(const char *session, slim_state *s) {
    char p[4096];
    memset(s, 0, sizeof *s);
    if (state_path(session, ".json", p, sizeof p)) return -1;
    char *data; size_t len;
    if (access(p, F_OK)) return slim_seterr("no session '%s' (run: slimium start --session %s)", session, session);
    if (read_file(p, &data, &len, 1 << 20)) return -1;
    jv d;
    if (json_parse(data, len, &d) || d.t != J_OBJ) { free(data); return slim_seterr("corrupt state file %s", p); }
    s->name = xstrdup(session);
    s->pid = (int)json_get_num(d, "pid", 0);
    s->port = (int)json_get_num(d, "port", 0);
    s->attached = (int)json_get_num(d, "attached", 0);
    s->headless = (int)json_get_num(d, "headless", 1);
    s->ws = json_get_str(d, "ws");
    s->target = json_get_str(d, "target");
    s->profile = json_get_str(d, "profile");
    s->user = json_get_str(d, "user");
    s->udd = json_get_str(d, "udd");
    s->engine = json_get_str(d, "engine");
    s->log = json_get_str(d, "log");
    s->vp_w = (int)json_get_num(d, "vp_w", 0);
    s->vp_h = (int)json_get_num(d, "vp_h", 0);
    s->vp_scale = json_get_num(d, "vp_scale", 0);
    s->vp_mobile = (int)json_get_num(d, "vp_mobile", 0);
    s->ua = json_get_str(d, "ua");
    s->offline = (int)json_get_num(d, "offline", 0);
    jv b;
    if (json_get(d, "block", &b) && b.t == J_ARR) s->block = xstrndup(b.p, b.n);
    s->dldir = json_get_str(d, "dldir");
    free(data);
    return 0;
}

static void kv_s(sbuf *b, const char *k, const char *v) {
    if (!v) return;
    sb_printf(b, ",\"%s\":", k); sb_jstrz(b, v);
}

int state_save(const slim_state *s) {
    char p[4096];
    if (state_path(s->name, ".json", p, sizeof p)) return -1;
    sbuf b = {0};
    sb_printf(&b, "{\"pid\":%d,\"port\":%d,\"attached\":%d,\"headless\":%d", s->pid, s->port, s->attached, s->headless);
    kv_s(&b, "ws", s->ws); kv_s(&b, "target", s->target); kv_s(&b, "profile", s->profile);
    kv_s(&b, "user", s->user); kv_s(&b, "udd", s->udd); kv_s(&b, "engine", s->engine); kv_s(&b, "log", s->log);
    if (s->vp_w > 0) sb_printf(&b, ",\"vp_w\":%d,\"vp_h\":%d,\"vp_scale\":%g,\"vp_mobile\":%d", s->vp_w, s->vp_h, s->vp_scale, s->vp_mobile);
    kv_s(&b, "ua", s->ua);
    if (s->offline) sb_puts(&b, ",\"offline\":1");
    if (s->block) { sb_puts(&b, ",\"block\":"); sb_puts(&b, s->block); }
    kv_s(&b, "dldir", s->dldir);
    sb_puts(&b, "}\n");
    int r = write_file(p, b.p, b.n, 1);
    sb_free(&b);
    return r;
}

int state_remove(const char *session) {
    char p[4096];
    if (state_path(session, ".json", p, sizeof p)) return -1;
    if (unlink(p) && access(p, F_OK) == 0) return slim_seterr("cannot remove %s", p);
    return 0;
}

int state_port_taken(int port, const char *except) {
    char d[3072];
    if (state_dir(d, sizeof d)) return 0;
    DIR *dir = opendir(d);
    if (!dir) return 0;
    struct dirent *e;
    int taken = 0;
    while (!taken && (e = readdir(dir))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        char name[128];
        if (l - 5 >= sizeof name) continue;
        memcpy(name, e->d_name, l - 5); name[l - 5] = 0;
        if (except && !strcmp(name, except)) continue;
        slim_state s;
        if (state_load(name, &s) == 0) { if (s.port == port) taken = 1; state_free(&s); }
    }
    closedir(dir);
    return taken;
}
