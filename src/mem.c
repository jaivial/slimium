/* PSS/RSS/USS of a process tree, from /proc. Linux only, libc + POSIX.
 * One scan of /proc builds a (pid, ppid) table in a single growable buffer;
 * descendants are marked by repeated passes (trees are shallow); each marked
 * process's /proc/<pid>/smaps_rollup is read into a stack buffer. */
#define _POSIX_C_SOURCE 200809L
#include "mem.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { pid_t pid, ppid; int in; } ent;

/* Read a small /proc file into buf (NUL-terminated). Returns bytes or -1. */
static long rd(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (long)n;
}

static void path_of(char *p, size_t cap, pid_t pid, const char *leaf) {
    /* "/proc/" + pid + "/" + leaf, without snprintf's locale machinery. */
    char num[24]; int k = 0; unsigned long v = (unsigned long)pid;
    do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 23);
    size_t o = 0; const char *pre = "/proc/";
    while (*pre && o + 1 < cap) p[o++] = *pre++;
    while (k && o + 1 < cap) p[o++] = num[--k];
    if (o + 1 < cap) p[o++] = '/';
    while (*leaf && o + 1 < cap) p[o++] = *leaf++;
    p[o] = 0;
}

static long field_kb(const char *s, const char *key) {
    size_t kl = strlen(key);
    for (const char *l = s; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL)
        if (!strncmp(l, key, kl)) return strtol(l + kl, NULL, 10);
    return 0;
}

int slim_mem_proc(pid_t pid, slim_mem *out) {
    char p[64], buf[4096];
    memset(out, 0, sizeof *out);
    path_of(p, sizeof p, pid, "smaps_rollup");
    if (rd(p, buf, sizeof buf) > 0) {
        out->rss_kb = field_kb(buf, "Rss:");
        out->pss_kb = field_kb(buf, "Pss:");
        out->uss_kb = field_kb(buf, "Private_Clean:") + field_kb(buf, "Private_Dirty:");
        out->procs = 1;
        return 0;
    }
    /* Not readable (non-dumpable) or gone: fall back to statm resident pages. */
    path_of(p, sizeof p, pid, "statm");
    if (rd(p, buf, sizeof buf) <= 0) return -1;
    char *e; strtol(buf, &e, 10);
    long res = strtol(e, NULL, 10), pg = sysconf(_SC_PAGESIZE) / 1024;
    out->rss_kb = out->pss_kb = res * (pg > 0 ? pg : 4);
    out->procs = 1;
    return 0;
}

/* ppid from /proc/<pid>/stat; comm may hold spaces and parens, so parse after
 * the last ')'. Returns -1 if the process is gone. */
static pid_t ppid_of(pid_t pid) {
    char p[64], buf[512];
    path_of(p, sizeof p, pid, "stat");
    if (rd(p, buf, sizeof buf) <= 0) return -1;
    char *r = strrchr(buf, ')');
    if (!r || r[1] != ' ' || !r[2] || r[3] != ' ') return -1;
    return (pid_t)strtol(r + 4, NULL, 10);
}

int slim_mem_tree_each(pid_t root, void (*cb)(pid_t, const slim_mem *, void *), void *ctx,
                       slim_mem *out) {
    memset(out, 0, sizeof *out);
    slim_mem m;
    if (slim_mem_proc(root, &m) != 0) return -1;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    ent *t = NULL; size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *s = de->d_name;
        if (*s < '1' || *s > '9') continue;
        char *e; long v = strtol(s, &e, 10);
        if (*e || (pid_t)v == root) continue;
        pid_t pp = ppid_of((pid_t)v);
        if (pp <= 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 512;
            ent *nt = realloc(t, nc * sizeof *nt);
            if (!nt) break;
            t = nt; cap = nc;
        }
        t[n].pid = (pid_t)v; t[n].ppid = pp; t[n].in = 0; n++;
    }
    closedir(d);
    /* Mark descendants: a pass that marks nothing new ends the walk. */
    for (int grew = 1; grew;) {
        grew = 0;
        for (size_t i = 0; i < n; i++) {
            if (t[i].in) continue;
            int hit = t[i].ppid == root;
            for (size_t j = 0; !hit && j < n; j++) hit = t[j].in && t[j].pid == t[i].ppid;
            if (hit) { t[i].in = 1; grew = 1; }
        }
    }
    if (cb) cb(root, &m, ctx);
    *out = m;
    for (size_t i = 0; i < n; i++) {
        if (!t[i].in || slim_mem_proc(t[i].pid, &m) != 0) continue;
        if (cb) cb(t[i].pid, &m, ctx);
        out->procs++; out->pss_kb += m.pss_kb; out->rss_kb += m.rss_kb; out->uss_kb += m.uss_kb;
    }
    free(t);
    return 0;
}

int slim_mem_tree(pid_t root, slim_mem *out) { return slim_mem_tree_each(root, NULL, NULL, out); }
