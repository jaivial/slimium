/* Detached screencast recording. Only this process talks to its CDP session and ffmpeg. */
#include "record.h"
#include "b64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REC_WAIT_MS 15000
#define REC_META_MAX (1 << 20)

typedef struct {
    char id[48], scope[96], session[80], status[16], error[256];
    char file[PATH_MAX], part[PATH_MAX], format[8], target[80], ffmpeg[PATH_MAX];
    int pid, ffmpeg_pid, port, fps, quality, max_seconds;
    int vp_w, vp_h, vp_mobile;
    double vp_scale;
    int width, height, source_width, source_height;
    long long started_ms, stopped_ms, frames, source_frames, bytes;
    unsigned long long start_ticks;
} rec_t;

static volatile sig_atomic_t rec_stop;
static void rec_signal(int sig) { (void)sig; rec_stop = 1; }

static int rec_scope(ctx_t *c, char out[96]) {
    int n = c->port ? snprintf(out, 96, "port-%d", c->port) : snprintf(out, 96, "%s", c->session);
    if (n < 0 || n >= 96) return slim_seterr("recording scope too long");
    return 0;
}

static int rec_id_ok(const char *id) {
    if (!id || strncmp(id, "r", 1) || strlen(id) < 8 || strlen(id) >= 48) return 0;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
    return 1;
}

static int rec_meta_path(const char *scope, const char *id, char *buf, size_t n) {
    if (!rec_id_ok(id)) return slim_seterr("bad recording id '%s'", id ? id : "");
    char d[3072];
    if (state_dir(d, sizeof d)) return -1;
    int k = snprintf(buf, n, "%s/record-%s-%s.json", d, scope, id);
    if (k < 0 || (size_t)k >= n) return slim_seterr("recording state path too long");
    return 0;
}

static void rec_json(sbuf *b, const rec_t *r) {
    sb_puts(b, "{\"id\":"); sb_jstrz(b, r->id);
    sb_puts(b, ",\"scope\":"); sb_jstrz(b, r->scope);
    sb_puts(b, ",\"session\":"); sb_jstrz(b, r->session);
    sb_puts(b, ",\"status\":"); sb_jstrz(b, r->status);
    sb_puts(b, ",\"error\":"); sb_jstrz(b, r->error);
    sb_puts(b, ",\"file\":"); sb_jstrz(b, r->file);
    sb_puts(b, ",\"part\":"); sb_jstrz(b, r->part);
    sb_printf(b, ",\"format\":\"%s\",\"target\":", r->format); sb_jstrz(b, r->target);
    sb_puts(b, ",\"ffmpeg\":"); sb_jstrz(b, r->ffmpeg);
    sb_printf(b, ",\"pid\":%d,\"ffmpeg_pid\":%d,\"port\":%d,\"fps\":%d,\"quality\":%d,\"max_seconds\":%d",
              r->pid, r->ffmpeg_pid, r->port, r->fps, r->quality, r->max_seconds);
    sb_printf(b, ",\"vp_w\":%d,\"vp_h\":%d,\"vp_mobile\":%d,\"vp_scale\":%g", r->vp_w, r->vp_h, r->vp_mobile, r->vp_scale);
    sb_printf(b, ",\"width\":%d,\"height\":%d,\"source_width\":%d,\"source_height\":%d",
              r->width, r->height, r->source_width, r->source_height);
    sb_printf(b, ",\"started_ms\":%lld,\"stopped_ms\":%lld,\"frames\":%lld,\"source_frames\":%lld,\"bytes\":%lld,\"start_ticks\":%llu}\n",
              r->started_ms, r->stopped_ms, r->frames, r->source_frames, r->bytes, r->start_ticks);
}

static int rec_save(const rec_t *r, const char *path) {
    sbuf b = {0};
    rec_json(&b, r);
    int rc = write_file(path, b.p, b.n, 1);
    sb_free(&b);
    return rc;
}

static void rec_str(char *dst, size_t n, jv doc, const char *key) {
    char *s = json_get_str(doc, key);
    if (s) { snprintf(dst, n, "%s", s); free(s); }
}

static int rec_load(rec_t *r, const char *path) {
    char *data = NULL; size_t len = 0;
    if (read_file(path, &data, &len, REC_META_MAX)) return -1;
    jv d;
    if (json_parse(data, len, &d) || d.t != J_OBJ) { free(data); return slim_seterr("corrupt recording state %s", path); }
    memset(r, 0, sizeof *r);
    rec_str(r->id, sizeof r->id, d, "id");
    rec_str(r->scope, sizeof r->scope, d, "scope");
    rec_str(r->session, sizeof r->session, d, "session");
    rec_str(r->status, sizeof r->status, d, "status");
    rec_str(r->error, sizeof r->error, d, "error");
    rec_str(r->file, sizeof r->file, d, "file");
    rec_str(r->part, sizeof r->part, d, "part");
    rec_str(r->format, sizeof r->format, d, "format");
    rec_str(r->target, sizeof r->target, d, "target");
    rec_str(r->ffmpeg, sizeof r->ffmpeg, d, "ffmpeg");
    r->pid = (int)json_get_num(d, "pid", 0);
    r->ffmpeg_pid = (int)json_get_num(d, "ffmpeg_pid", 0);
    r->port = (int)json_get_num(d, "port", 0);
    r->fps = (int)json_get_num(d, "fps", 10);
    r->quality = (int)json_get_num(d, "quality", 75);
    r->max_seconds = (int)json_get_num(d, "max_seconds", 0);
    r->vp_w = (int)json_get_num(d, "vp_w", 0);
    r->vp_h = (int)json_get_num(d, "vp_h", 0);
    r->vp_mobile = (int)json_get_num(d, "vp_mobile", 0);
    r->vp_scale = json_get_num(d, "vp_scale", 0);
    r->width = (int)json_get_num(d, "width", 0);
    r->height = (int)json_get_num(d, "height", 0);
    r->source_width = (int)json_get_num(d, "source_width", 0);
    r->source_height = (int)json_get_num(d, "source_height", 0);
    r->started_ms = (long long)json_get_num(d, "started_ms", 0);
    r->stopped_ms = (long long)json_get_num(d, "stopped_ms", 0);
    r->frames = (long long)json_get_num(d, "frames", 0);
    r->source_frames = (long long)json_get_num(d, "source_frames", 0);
    r->bytes = (long long)json_get_num(d, "bytes", 0);
    jv t;
    if (json_get(d, "start_ticks", &t)) r->start_ticks = strtoull(t.p, NULL, 10);
    free(data);
    if (!rec_id_ok(r->id)) return slim_seterr("invalid recording state");
    return 0;
}

static int proc_identity(pid_t pid, unsigned long long *ticks, char *state) {
    char path[64], buf[4096]; size_t len = 0;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    char *data = NULL;
    if (read_file(path, &data, &len, sizeof buf)) return -1;
    char *end = strrchr(data, ')');
    if (!end || !end[1]) { free(data); return -1; }
    char *p = end + 2, *save = NULL;
    int field = 3, ok = 0;
    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 3 && state) *state = tok[0];
        if (field == 22) {
            char *e = NULL; errno = 0;
            unsigned long long v = strtoull(tok, &e, 10);
            if (!errno && e && !*e) { *ticks = v; ok = 1; }
            break;
        }
    }
    free(data);
    return ok ? 0 : -1;
}

static int rec_alive(const rec_t *r) {
    if (r->pid <= 0 || !r->start_ticks) return 0;
    unsigned long long ticks = 0; char state = 0;
    return proc_identity((pid_t)r->pid, &ticks, &state) == 0 && ticks == r->start_ticks && state != 'Z';
}

/* Only a same-boot slimium worker with the exact recorded pid/start time may be signaled. */
static int rec_validate_worker(const rec_t *r) {
    if (!rec_alive(r)) return 0;
    char p1[64], p2[] = "/proc/self/exe";
    struct stat a, b;
    snprintf(p1, sizeof p1, "/proc/%d/exe", r->pid);
    if (stat(p1, &a) || stat(p2, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return 0;
    snprintf(p1, sizeof p1, "/proc/%d/cmdline", r->pid);
    char *cmd = NULL; size_t len = 0;
    if (read_file(p1, &cmd, &len, 4096)) return 0;
    int ok = memmem(cmd, len, "__record-worker", 15) != NULL && memmem(cmd, len, r->id, strlen(r->id)) != NULL;
    free(cmd);
    return ok;
}

static int rec_output(ctx_t *c, const rec_t *r) {
    out_s(c, "id", r->id); out_s(c, "status", r->status); out_s(c, "file", r->file);
    out_s(c, "format", r->format); out_i(c, "pid", r->pid); out_i(c, "fps", r->fps);
    out_i(c, "frames", r->frames); out_i(c, "source_frames", r->source_frames);
    out_d(c, "duration", r->fps > 0 ? (double)r->frames / r->fps : 0);
    out_i(c, "bytes", r->bytes); out_b(c, "audio", 0);
    if (r->width) { out_i(c, "width", r->width); out_i(c, "height", r->height); }
    if (r->error[0]) out_s(c, "error", r->error);
    return 0;
}

static void rec_json_item(sbuf *b, const rec_t *r, int *n) {
    if ((*n)++) sb_putc(b, ',');
    sb_puts(b, "{\"id\":"); sb_jstrz(b, r->id);
    sb_puts(b, ",\"status\":"); sb_jstrz(b, r->status);
    sb_puts(b, ",\"file\":"); sb_jstrz(b, r->file);
    sb_printf(b, ",\"pid\":%d,\"alive\":%s,\"fps\":%d,\"frames\":%lld,\"source_frames\":%lld,\"duration\":%.3f,\"bytes\":%lld",
              r->pid, rec_alive(r) ? "true" : "false", r->fps, r->frames, r->source_frames,
              r->fps > 0 ? (double)r->frames / r->fps : 0, r->bytes);
    if (r->error[0]) { sb_puts(b, ",\"error\":"); sb_jstrz(b, r->error); }
    sb_putc(b, '}');
}

static int rec_collect(ctx_t *c, rec_t **out, char ***paths, int *count) {
    char scope[96], d[3072], prefix[160];
    if (rec_scope(c, scope) || state_dir(d, sizeof d)) return -1;
    snprintf(prefix, sizeof prefix, "record-%s-", scope);
    DIR *dir = opendir(d);
    if (!dir) return slim_seterr("open recording state: %s", strerror(errno));
    rec_t *items = NULL; char **names = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t ln = strlen(e->d_name), pl = strlen(prefix);
        if (ln <= pl + 5 || strncmp(e->d_name, prefix, pl) || strcmp(e->d_name + ln - 5, ".json")) continue;
        char path[PATH_MAX];
        int k = snprintf(path, sizeof path, "%s/%s", d, e->d_name);
        if (k < 0 || (size_t)k >= sizeof path) continue;
        rec_t r;
        if (rec_load(&r, path)) continue;
        if (strcmp(r.scope, scope)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            items = xrealloc(items, (size_t)cap * sizeof *items);
            names = xrealloc(names, (size_t)cap * sizeof *names);
        }
        items[n] = r; names[n] = xstrdup(path); n++;
    }
    closedir(dir);
    *out = items; *paths = names; *count = n;
    return 0;
}

static void rec_free_collection(rec_t *items, char **paths, int n) {
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths); free(items);
}

static int rec_list(ctx_t *c, int status_only) {
    rec_t *items = NULL; char **paths = NULL; int n = 0;
    if (rec_collect(c, &items, &paths, &n)) return -1;
    sbuf b = {0}; sb_putc(&b, '['); int shown = 0;
    for (int i = 0; i < n; i++) {
        int active = !strcmp(items[i].status, "recording") || !strcmp(items[i].status, "starting");
        if (status_only && !active) continue;
        rec_json_item(&b, &items[i], &shown);
        out_t(c, "%s%s\t%s\t%s\t%lld frames\t%.2fs%s%s", shown > 1 ? "\n" : "", items[i].id,
              items[i].status, items[i].file, items[i].frames,
              items[i].fps ? (double)items[i].frames / items[i].fps : 0,
              items[i].error[0] ? "\t" : "", items[i].error);
    }
    sb_putc(&b, ']');
    out_raw(c, "recordings", b.p, b.n); out_i(c, "count", shown);
    sb_free(&b); rec_free_collection(items, paths, n);
    return 0;
}

static int absolute_output(const char *in, char out[PATH_MAX]) {
    if (!in || !*in || !strcmp(in, "-")) return slim_seterr("record output must be a file path");
    int k;
    if (in[0] == '/') k = snprintf(out, PATH_MAX, "%s", in);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return slim_seterr("getcwd: %s", strerror(errno));
        k = snprintf(out, PATH_MAX, "%s/%s", cwd, in);
    }
    if (k < 0 || k >= PATH_MAX) return slim_seterr("output path too long");
    return 0;
}

static int rec_start(ctx_t *c, int argc, char **argv) {
    const char *fps_s = opt_val(c, &argc, argv, "--fps");
    const char *fmt = opt_val(c, &argc, argv, "--format");
    const char *max_s = opt_val(c, &argc, argv, "--max-seconds");
    const char *quality_s = opt_val(c, &argc, argv, "--quality");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "record start FILE [--tab ID] [--fps N] [--format mp4|webm|avi] [--max-seconds N] [--quality N]");
    rec_t r; memset(&r, 0, sizeof r);
    if (rec_scope(c, r.scope) || absolute_output(argv[0], r.file)) return -1;
    snprintf(r.id, sizeof r.id, "r%llx-%d", (unsigned long long)now_ms(), (int)getpid());
    if (!c->port) snprintf(r.session, sizeof r.session, "%s", c->session);
    r.fps = 10; r.quality = 75;
    long v;
    if (fps_s && (parse_int(fps_s, &v) || v < 1 || v > 60)) return usage_err(c, "bad --fps '%s' (1-60)", fps_s);
    if (fps_s) r.fps = (int)v;
    if (quality_s && (parse_int(quality_s, &v) || v < 0 || v > 100)) return usage_err(c, "bad --quality '%s' (0-100)", quality_s);
    if (quality_s) r.quality = (int)v;
    if (max_s && (parse_int(max_s, &v) || v < 1 || v > 86400)) return usage_err(c, "bad --max-seconds '%s' (1-86400)", max_s);
    if (max_s) r.max_seconds = (int)v;
    if (!fmt) {
        const char *dot = strrchr(r.file, '.');
        fmt = dot && !strcasecmp(dot, ".webm") ? "webm" : dot && !strcasecmp(dot, ".avi") ? "avi" : "mp4";
    }
    if (strcmp(fmt, "mp4") && strcmp(fmt, "webm") && strcmp(fmt, "avi")) return usage_err(c, "bad --format '%s'", fmt);
    snprintf(r.format, sizeof r.format, "%s", fmt);
    if (snprintf(r.part, sizeof r.part, "%s.%s.part.%s", r.file, r.id, fmt) >= (int)sizeof r.part) return slim_seterr("output path too long");
    char *ff = access("/usr/bin/ffmpeg", X_OK) == 0 ? xstrdup("/usr/bin/ffmpeg") : find_in_path("ffmpeg");
    if (!ff) return slim_seterr("ffmpeg is required at runtime for record (not linked)");
    snprintf(r.ffmpeg, sizeof r.ffmpeg, "%s", ff); free(ff);
    if (ctx_page(c)) return -1;
    r.port = c->port ? c->port : c->st.port;
    snprintf(r.target, sizeof r.target, "%s", c->tid);
    if (c->have_st) {
        r.vp_w = c->st.vp_w; r.vp_h = c->st.vp_h; r.vp_mobile = c->st.vp_mobile; r.vp_scale = c->st.vp_scale;
    }
    snprintf(r.status, sizeof r.status, "starting");
    char meta[PATH_MAX];
    if (rec_meta_path(r.scope, r.id, meta, sizeof meta) || rec_save(&r, meta)) return -1;
    int syncfd[2];
    if (pipe(syncfd)) { unlink(meta); return slim_seterr("pipe: %s", strerror(errno)); }
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno; close(syncfd[0]); close(syncfd[1]); unlink(meta);
        return slim_seterr("fork recorder: %s", strerror(e));
    }
    if (pid == 0) {
        close(syncfd[1]);
        char logpath[PATH_MAX];
        if (snprintf(logpath, sizeof logpath, "%s.log", meta) >= (int)sizeof logpath) _exit(125);
        int log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        if (log >= 0) { dup2(log, 1); dup2(log, 2); if (log > 2) close(log); }
        setsid();
        char byte;
        ssize_t got;
        do got = read(syncfd[0], &byte, 1); while (got < 0 && errno == EINTR);
        close(syncfd[0]);
        if (got != 1) _exit(126);
        execl("/proc/self/exe", "slimium", "__record-worker", r.id, meta, (char *)NULL);
        rec_t bad; memset(&bad, 0, sizeof bad);
        if (rec_load(&bad, meta) == 0) {
            snprintf(bad.status, sizeof bad.status, "error");
            snprintf(bad.error, sizeof bad.error, "exec recorder: %s", strerror(errno));
            rec_save(&bad, meta);
        }
        _exit(127);
    }
    close(syncfd[0]);
    r.pid = (int)pid;
    proc_identity(pid, &r.start_ticks, NULL);
    int saved = rec_save(&r, meta);
    char byte = 'R';
    if (write(syncfd[1], &byte, 1) != 1 && !saved) saved = -1;
    close(syncfd[1]);
    if (saved) {
        if (rec_validate_worker(&r)) kill(pid, SIGTERM);
        unlink(meta);
        return -1;
    }
    long long deadline = now_ms() + (c->timeout > 0 && c->timeout < REC_WAIT_MS ? c->timeout : REC_WAIT_MS);
    int rc = -1;
    for (;;) {
        rec_t cur;
        if (rec_load(&cur, meta) == 0) {
            if (!strcmp(cur.status, "recording")) { rec_output(c, &cur); out_t(c, "%s\t%s", cur.id, cur.file); rc = 0; break; }
            if (!strcmp(cur.status, "error") || !strcmp(cur.status, "stopped")) {
                slim_seterr("record start failed: %s", cur.error[0] ? cur.error : "recorder exited");
                break;
            }
        }
        if (!rec_alive(&r)) { slim_seterr("record start failed: recorder exited during startup"); break; }
        if (now_ms() >= deadline) {
            if (rec_validate_worker(&r)) kill(pid, SIGTERM);
            slim_seterr("record start timed out");
            break;
        }
        sleep_ms(25);
    }
    if (rc) waitpid(pid, NULL, WNOHANG);
    return rc;
}

static int rec_stop_cmd(ctx_t *c, int argc, char **argv) {
    const char *id = opt_val(c, &argc, argv, "--id");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "record stop [--id ID]");
    char scope[96], meta[PATH_MAX];
    if (rec_scope(c, scope)) return -1;
    rec_t r;
    if (id) {
        if (rec_meta_path(scope, id, meta, sizeof meta) || rec_load(&r, meta)) return -1;
    } else {
        rec_t *items = NULL; char **paths = NULL; int n = 0, found = -1;
        if (rec_collect(c, &items, &paths, &n)) return -1;
        for (int i = 0; i < n; i++)
            if ((!strcmp(items[i].status, "recording") || !strcmp(items[i].status, "starting")) && rec_alive(&items[i])) {
                if (found >= 0) { rec_free_collection(items, paths, n); return slim_seterr("multiple active recordings; use --id ID"); }
                found = i;
            }
        if (found < 0) { rec_free_collection(items, paths, n); return slim_seterr("no active recording"); }
        r = items[found]; snprintf(meta, sizeof meta, "%s", paths[found]);
        rec_free_collection(items, paths, n);
    }
    if (!rec_alive(&r)) return slim_seterr("recording %s is not running", r.id);
    if (!rec_validate_worker(&r)) return slim_seterr("refusing to signal unverified recorder pid %d", r.pid);
    if (kill((pid_t)r.pid, SIGTERM)) return slim_seterr("stop recorder %d: %s", r.pid, strerror(errno));
    long long deadline = now_ms() + REC_WAIT_MS;
    for (;;) {
        rec_t cur;
        if (rec_load(&cur, meta) == 0 && (!strcmp(cur.status, "stopped") || !strcmp(cur.status, "error")) && !rec_alive(&cur)) {
            if (!strcmp(cur.status, "error")) { rec_output(c, &cur); return slim_seterr("record %s: %s", cur.id, cur.error); }
            rec_output(c, &cur); out_t(c, "%s\t%s\t%lld frames\t%.2fs", cur.id, cur.file, cur.frames, cur.fps ? (double)cur.frames / cur.fps : 0);
            return 0;
        }
        if (now_ms() >= deadline) return slim_seterr("timeout finalizing recording %s", r.id);
        sleep_ms(50);
    }
}

int record_command(ctx_t *c, int argc, char **argv) {
    if (argc < 1) return usage_err(c, "record start FILE | stop [--id ID] | status | list");
    if (!strcmp(argv[0], "start")) return rec_start(c, argc - 1, argv + 1);
    if (!strcmp(argv[0], "stop")) return rec_stop_cmd(c, argc - 1, argv + 1);
    if (!strcmp(argv[0], "status") || !strcmp(argv[0], "list")) {
        int status = !strcmp(argv[0], "status");
        if (opt_done(c, &argc, argv)) return -1;
        if (argc != 1) return usage_err(c, "record %s takes no arguments", argv[0]);
        return rec_list(c, status);
    }
    return usage_err(c, "unknown record action '%s'", argv[0]);
}

static int image_size(const unsigned char *p, size_t n, int *w, int *h) {
    if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        *w = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
        *h = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
        return *w > 0 && *h > 0 ? 0 : -1;
    }
    if (n >= 4 && p[0] == 0xff && p[1] == 0xd8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (p[i] != 0xff) { i++; continue; }
            while (i < n && p[i] == 0xff) i++;
            if (i >= n) break;
            unsigned marker = p[i++];
            if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (i + 2 > n) break;
            unsigned len = ((unsigned)p[i] << 8) | p[i + 1];
            if (len < 2 || i + len > n) break;
            if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
                if (len >= 7) {
                    *h = (p[i + 3] << 8) | p[i + 4]; *w = (p[i + 5] << 8) | p[i + 6];
                    return *w > 0 && *h > 0 ? 0 : -1;
                }
            }
            i += len;
        }
    }
    return -1;
}

static int apply_rec_viewport(cdp_t *cdp, const char *sid, const rec_t *r, int timeout) {
    if (r->vp_w <= 0) return cdp_call(cdp, sid, "Emulation.clearDeviceMetricsOverride", NULL, timeout, NULL, NULL);
    char p[192];
    snprintf(p, sizeof p, "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%g,\"mobile\":%s}",
             r->vp_w, r->vp_h, r->vp_scale, r->vp_mobile ? "true" : "false");
    return cdp_call(cdp, sid, "Emulation.setDeviceMetricsOverride", p, timeout, NULL, NULL);
}

static int refresh_rec_viewport(cdp_t *cdp, const char *sid, rec_t *r) {
    if (!r->session[0]) return 0;
    slim_state s;
    if (state_load(r->session, &s)) return 0;
    int changed = s.vp_w != r->vp_w || s.vp_h != r->vp_h || s.vp_mobile != r->vp_mobile || s.vp_scale != r->vp_scale;
    if (changed) {
        r->vp_w = s.vp_w; r->vp_h = s.vp_h; r->vp_mobile = s.vp_mobile; r->vp_scale = s.vp_scale;
        if (apply_rec_viewport(cdp, sid, r, 2000)) { state_free(&s); return -1; }
    }
    state_free(&s);
    return 0;
}

static int write_all(int fd, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return slim_seterr("ffmpeg input: %s", strerror(errno)); }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int ffmpeg_finish(pid_t pid) {
    int status = 0, done = 0;
    for (int i = 0; i < 150; i++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { done = 1; break; }
        if (w < 0) return -1;
        sleep_ms(100);
    }
    if (!done) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { done = 1; break; }
            sleep_ms(100);
        }
    }
    if (!done) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    return done && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static pid_t ffmpeg_start(rec_t *r, int *input_fd) {
    int fds[2];
    if (pipe(fds)) { slim_seterr("ffmpeg pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); slim_seterr("fork ffmpeg: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        close(fds[1]);
        if (dup2(fds[0], 0) < 0) _exit(126);
        close(fds[0]);
        char fps[16], crf[16], filter[256], qscale[16];
        snprintf(fps, sizeof fps, "%d", r->fps);
        snprintf(filter, sizeof filter, "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,setsar=1",
                 r->width, r->height, r->width, r->height);
        char *args[40]; int n = 0;
        args[n++] = (char *)"ffmpeg"; args[n++] = (char *)"-hide_banner"; args[n++] = (char *)"-loglevel"; args[n++] = (char *)"error";
        args[n++] = (char *)"-nostdin"; args[n++] = (char *)"-y";
        args[n++] = (char *)"-f"; args[n++] = (char *)"image2pipe"; args[n++] = (char *)"-framerate"; args[n++] = fps; args[n++] = (char *)"-i"; args[n++] = (char *)"pipe:0";
        args[n++] = (char *)"-an"; args[n++] = (char *)"-vf"; args[n++] = filter;
        if (!strcmp(r->format, "mp4")) {
            snprintf(crf, sizeof crf, "%d", 51 - r->quality * 34 / 100);
            args[n++] = (char *)"-c:v"; args[n++] = (char *)"libx264"; args[n++] = (char *)"-preset"; args[n++] = (char *)"veryfast";
            args[n++] = (char *)"-crf"; args[n++] = crf; args[n++] = (char *)"-pix_fmt"; args[n++] = (char *)"yuv420p";
            args[n++] = (char *)"-movflags"; args[n++] = (char *)"+faststart";
        } else if (!strcmp(r->format, "webm")) {
            snprintf(crf, sizeof crf, "%d", 63 - r->quality * 63 / 100);
            args[n++] = (char *)"-c:v"; args[n++] = (char *)"libvpx-vp9"; args[n++] = (char *)"-deadline"; args[n++] = (char *)"realtime";
            args[n++] = (char *)"-cpu-used"; args[n++] = (char *)"5"; args[n++] = (char *)"-crf"; args[n++] = crf;
            args[n++] = (char *)"-b:v"; args[n++] = (char *)"0"; args[n++] = (char *)"-pix_fmt"; args[n++] = (char *)"yuv420p";
        } else {
            snprintf(qscale, sizeof qscale, "%d", 1 + (100 - r->quality) * 30 / 100);
            args[n++] = (char *)"-c:v"; args[n++] = (char *)"mpeg4"; args[n++] = (char *)"-q:v"; args[n++] = qscale;
            args[n++] = (char *)"-pix_fmt"; args[n++] = (char *)"yuv420p";
        }
        args[n++] = r->part; args[n] = NULL;
        execv(r->ffmpeg, args);
        _exit(127);
    }
    close(fds[0]); *input_fd = fds[1];
    return pid;
}

static int take_frame(cdp_t *cdp, const char *sid, char *msg, jv params,
                      unsigned char **latest, size_t *latest_n, rec_t *r) {
    long long session = (long long)json_get_num(params, "sessionId", 0);
    char ack[80];
    snprintf(ack, sizeof ack, "{\"sessionId\":%lld}", session);
    if (cdp_send(cdp, sid, "Page.screencastFrameAck", ack, NULL)) { free(msg); return -1; }
    jv d;
    if (!json_get(params, "data", &d) || d.t != J_STR) { free(msg); return slim_seterr("screencast frame without data"); }
    char *s = (char *)d.p + 1; size_t n = d.n - 2;
    if (memchr(s, '\\', n)) { long u = json_unescape(s, n, s); if (u < 0) { free(msg); return slim_seterr("bad screencast data"); } n = (size_t)u; }
    long decoded = b64_decode(s, n, (unsigned char *)s);
    if (decoded <= 0) { free(msg); return slim_seterr("bad screencast base64"); }
    memmove(msg, s, (size_t)decoded);
    free(*latest); *latest = (unsigned char *)msg; *latest_n = (size_t)decoded;
    int w = 0, h = 0;
    if (image_size(*latest, *latest_n, &w, &h) == 0) { r->source_width = w; r->source_height = h; }
    r->source_frames++;
    return 0;
}

int record_worker_command(ctx_t *c, int argc, char **argv) {
    (void)c;
    if (argc != 2) return slim_seterr("internal recorder arguments");
    const char *id = argv[0], *meta = argv[1];
    char statedir[3072], prefix[3200];
    if (state_dir(statedir, sizeof statedir)) return -1;
    snprintf(prefix, sizeof prefix, "%s/record-", statedir);
    if (!rec_id_ok(id) || strncmp(meta, prefix, strlen(prefix))) return slim_seterr("invalid internal recorder state");
    rec_t r;
    if (rec_load(&r, meta) || strcmp(r.id, id) || r.pid != getpid()) return slim_seterr("recorder state mismatch");
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = rec_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    cdp_t cdp; memset(&cdp, 0, sizeof cdp); cdp.w.fd = -1;
    char *url = NULL, *sid = NULL, *latest = NULL; size_t latest_n = 0;
    int ffin = -1; pid_t ff_pid = -1;
    const char *failure = NULL;
    long long next_save = 0, next_vp = 0;
    double next_tick = 0;

    if (cdp_browser_url(r.port, 5000, &url, NULL) || cdp_connect(&cdp, url, 5000)) { failure = slim_errmsg(); goto done; }
    if (cdp_attach(&cdp, r.target, 5000, &sid) || apply_rec_viewport(&cdp, sid, &r, 5000)) { failure = slim_errmsg(); goto done; }
    {
        char start[96];
        snprintf(start, sizeof start, "{\"format\":\"jpeg\",\"quality\":%d,\"everyNthFrame\":1}", r.quality);
        if (cdp_call(&cdp, sid, "Page.startScreencast", start, 5000, NULL, NULL)) { failure = slim_errmsg(); goto done; }
    }
    while (!rec_stop) {
        long long now = now_ms();
        if (r.started_ms && r.max_seconds && now - r.started_ms >= (long long)r.max_seconds * 1000) break;
        if (now >= next_vp) {
            if (refresh_rec_viewport(&cdp, sid, &r)) { failure = slim_errmsg(); goto done; }
            next_vp = now + 250;
        }
        if (latest && r.started_ms && now >= next_tick) {
            double interval = 1000.0 / r.fps;
            int catchup = 0;
            do {
                if (write_all(ffin, (unsigned char *)latest, latest_n)) { failure = slim_errmsg(); goto done; }
                r.frames++; next_tick += interval;
            } while (next_tick <= now && ++catchup < r.fps);
            if (next_tick <= now) next_tick = now + interval;
        }
        if (r.started_ms && now >= next_save) {
            rec_save(&r, meta); next_save = now + 1000;
        }
        int wait = 100;
        if (r.started_ms) {
            double left = next_tick - now_ms();
            wait = left <= 0 ? 0 : left < 100 ? (int)(left + 0.999) : 100;
        }
        char *msg = NULL; jv params;
        int ev = cdp_event(&cdp, sid, "Page.screencastFrame", wait, &msg, &params, NULL);
        if (ev < 0) { failure = slim_errmsg(); goto done; }
        if (ev == 0) continue;
        if (take_frame(&cdp, sid, msg, params, (unsigned char **)&latest, &latest_n, &r)) { failure = slim_errmsg(); goto done; }
        if (!r.started_ms) {
            if (!r.source_width || !r.source_height) { failure = "could not determine screencast image dimensions"; goto done; }
            r.width = (r.source_width + 1) & ~1; r.height = (r.source_height + 1) & ~1;
            ff_pid = ffmpeg_start(&r, &ffin);
            if (ff_pid < 0) { failure = slim_errmsg(); goto done; }
            r.ffmpeg_pid = (int)ff_pid; r.started_ms = now_ms(); next_tick = r.started_ms;
            snprintf(r.status, sizeof r.status, "recording");
            if (rec_save(&r, meta)) { failure = slim_errmsg(); goto done; }
        }
    }

done:
    char failure_copy[256] = {0};
    if (failure) snprintf(failure_copy, sizeof failure_copy, "%s", failure);
    failure = failure_copy[0] ? failure_copy : NULL;
    if (sid && cdp.open) cdp_call(&cdp, sid, "Page.stopScreencast", NULL, 1000, NULL, NULL);
    if (ffin >= 0) close(ffin);
    int ff_ok = 1;
    if (ff_pid > 0) ff_ok = ffmpeg_finish(ff_pid) == 0;
    r.ffmpeg_pid = 0;
    r.stopped_ms = now_ms();
    struct stat st;
    if (ff_pid > 0 && ff_ok && r.frames > 0 && stat(r.part, &st) == 0) {
        if (rename(r.part, r.file) == 0) {
            r.bytes = (long long)st.st_size;
            snprintf(r.status, sizeof r.status, "stopped");
            if (failure && !r.error[0]) snprintf(r.error, sizeof r.error, "%.230s; recording finalized", failure);
        } else {
            snprintf(r.status, sizeof r.status, "error");
            snprintf(r.error, sizeof r.error, "rename finalized video: %s", strerror(errno));
        }
    } else {
        snprintf(r.status, sizeof r.status, "error");
        if (failure) snprintf(r.error, sizeof r.error, "%s", failure);
        else if (!r.frames) snprintf(r.error, sizeof r.error, "no frames recorded");
        else snprintf(r.error, sizeof r.error, "ffmpeg encoding failed (partial file retained)");
    }
    if (cdp.open) cdp_close(&cdp);
    free(sid); free(url); free(latest);
    int save_rc = rec_save(&r, meta);
    return save_rc || !strcmp(r.status, "error") ? -1 : 0;
}
