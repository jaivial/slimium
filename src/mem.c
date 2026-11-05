/* Memory of a process tree, from /proc. Linux only, libc + POSIX.
 * One scan of /proc builds a (pid, ppid) table in a single growable buffer;
 * descendants are marked by repeated passes (trees are shallow). Each marked
 * process's smaps_rollup gives the PSS split; its maps + pagemap give the
 * resident file pages, recorded in one bitmap per (device, inode) so a page
 * shared by many processes of the tree is counted once (file_union). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mem.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

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

/* ---- file page union -------------------------------------------------------- */

typedef struct { unsigned long long dev, ino; unsigned char *bits; size_t nbytes; } fmap;
typedef struct { fmap *f; size_t n, cap; unsigned long long devs[32]; signed char tmp[32]; int ndev; long pages; } funion;

/* Files on tmpfs (and memfd / SysV / device mappings) are shmem: already in
 * Pss_Shmem, so they stay out of the file union. Cached per device. */
static int is_shmem(funion *u, unsigned long long dev, const char *path) {
    if (!strncmp(path, "/memfd:", 7) || !strncmp(path, "/SYSV", 5) || !strncmp(path, "/dev/", 5)) return 1;
    for (int i = 0; i < u->ndev; i++) if (u->devs[i] == dev) return u->tmp[i];
    struct statfs s; int t;
    if (statfs(path, &s) == 0) t = (unsigned long)s.f_type == (unsigned long)TMPFS_MAGIC;
    else return strstr(path, " (deleted)") != NULL;   /* unknown: do not cache */
    if (u->ndev < 32) { u->devs[u->ndev] = dev; u->tmp[u->ndev++] = (signed char)t; }
    return t;
}

static fmap *file_of(funion *u, unsigned long long dev, unsigned long long ino) {
    for (size_t i = 0; i < u->n; i++) if (u->f[i].dev == dev && u->f[i].ino == ino) return &u->f[i];
    if (u->n == u->cap) {
        size_t nc = u->cap ? u->cap * 2 : 128;
        fmap *nf = realloc(u->f, nc * sizeof *nf);
        if (!nf) return NULL;
        u->f = nf; u->cap = nc;
    }
    fmap *m = &u->f[u->n++];
    m->dev = dev; m->ino = ino; m->bits = NULL; m->nbytes = 0;
    return m;
}

/* Mark page index pg of file m; returns 1 if it was not yet counted. */
static int mark(fmap *m, unsigned long long pg) {
    size_t byte = (size_t)(pg >> 3);
    if (byte >= m->nbytes) {
        size_t nb = m->nbytes ? m->nbytes : 1024;
        while (nb <= byte) nb *= 2;
        unsigned char *b = realloc(m->bits, nb);
        if (!b) return 0;
        memset(b + m->nbytes, 0, nb - m->nbytes);
        m->bits = b; m->nbytes = nb;
    }
    unsigned char bit = (unsigned char)(1u << (pg & 7));
    if (m->bits[byte] & bit) return 0;
    m->bits[byte] |= bit;
    return 1;
}

static void funion_free(funion *u) {
    for (size_t i = 0; i < u->n; i++) free(u->f[i].bits);
    free(u->f);
}

/* Resident file pages of pid: adds new ones to u->pages; returns how many
 * resident file pages pid itself maps (its own, not deduplicated vs others). */
static long scan_files(pid_t pid, funion *u) {
    char p[64], buf[8192];
    path_of(p, sizeof p, pid, "maps");
    int mfd = open(p, O_RDONLY | O_CLOEXEC);
    if (mfd < 0) return 0;
    path_of(p, sizeof p, pid, "pagemap");
    int pfd = open(p, O_RDONLY | O_CLOEXEC);
    if (pfd < 0) { close(mfd); return 0; }
    long own = 0, psz = sysconf(_SC_PAGESIZE);
    if (psz <= 0) psz = 4096;
    size_t have = 0;
    for (;;) {
        ssize_t r = read(mfd, buf + have, sizeof buf - 1 - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 && !have) break;
        if (r > 0) have += (size_t)r;
        buf[have] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            /* start-end perms offset maj:min inode path */
            char *e;
            unsigned long long start = strtoull(line, &e, 16), end = strtoull(e + 1, &e, 16);
            while (*e == ' ') e++;
            while (*e && *e != ' ') e++;                          /* perms */
            unsigned long long off = strtoull(e, &e, 16);
            unsigned long long maj = strtoull(e, &e, 16), min = strtoull(e + 1, &e, 16);
            unsigned long long ino = strtoull(e, &e, 10);
            while (*e == ' ') e++;
            if (ino && *e == '/' && end > start && !is_shmem(u, maj << 32 | min, e)) {
                fmap *m = file_of(u, maj << 32 | min, ino);
                uint64_t pm[512];
                unsigned long long first = start / (unsigned long long)psz, npg = (end - start) / (unsigned long long)psz;
                for (unsigned long long i = 0; m && i < npg;) {
                    size_t want = npg - i < 512 ? (size_t)(npg - i) : 512;
                    ssize_t got = pread(pfd, pm, want * 8, (off_t)((first + i) * 8));
                    if (got <= 0) break;
                    size_t k = (size_t)got / 8;
                    for (size_t j = 0; j < k; j++)
                        /* bit 63 present, bit 61 file page (not a private COW copy) */
                        if ((pm[j] >> 63 & 1) && (pm[j] >> 61 & 1)) {
                            own++;
                            u->pages += mark(m, off / (unsigned long long)psz + i + j);
                        }
                    i += k;
                }
            }
            line = nl + 1;
        }
        have = strlen(line);
        memmove(buf, line, have);
        if (have >= sizeof buf - 1) have = 0;   /* absurdly long line: drop it */
        if (r <= 0) break;
    }
    close(pfd); close(mfd);
    return own * (psz / 1024);
}

/* ---- per process ----------------------------------------------------------- */

static int proc_mem(pid_t pid, slim_mem *out, funion *u) {
    char p[64], buf[4096];
    memset(out, 0, sizeof *out);
    path_of(p, sizeof p, pid, "smaps_rollup");
    if (rd(p, buf, sizeof buf) > 0) {
        out->rss_kb = field_kb(buf, "Rss:");
        out->pss_kb = field_kb(buf, "Pss:");
        out->uss_kb = field_kb(buf, "Private_Clean:") + field_kb(buf, "Private_Dirty:");
        out->anon_kb = field_kb(buf, "Pss_Anon:");
        out->shmem_kb = field_kb(buf, "Pss_Shmem:");
        out->file_kb = field_kb(buf, "Pss_File:");
        out->swap_kb = field_kb(buf, "SwapPss:");
        out->procs = 1;
        if (u) out->file_union_kb = scan_files(pid, u);
        else { funion one = {0}; out->file_union_kb = scan_files(pid, &one); funion_free(&one); }
        out->iso_kb = out->anon_kb + out->shmem_kb + out->file_union_kb;
        return 0;
    }
    /* Not readable (non-dumpable) or gone: fall back to statm resident pages. */
    path_of(p, sizeof p, pid, "statm");
    if (rd(p, buf, sizeof buf) <= 0) return -1;
    char *e; strtol(buf, &e, 10);
    long res = strtol(e, NULL, 10), pg = sysconf(_SC_PAGESIZE) / 1024;
    out->rss_kb = out->pss_kb = out->iso_kb = res * (pg > 0 ? pg : 4);
    out->procs = 1;
    return 0;
}

int slim_mem_proc(pid_t pid, slim_mem *out) { return proc_mem(pid, out, NULL); }

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

static void add(slim_mem *t, const slim_mem *m) {
    t->procs += m->procs; t->pss_kb += m->pss_kb; t->rss_kb += m->rss_kb; t->uss_kb += m->uss_kb;
    t->anon_kb += m->anon_kb; t->shmem_kb += m->shmem_kb; t->file_kb += m->file_kb; t->swap_kb += m->swap_kb;
}

int slim_mem_trees(const pid_t *roots, int nroots, void (*cb)(pid_t, const slim_mem *, void *), void *ctx,
                   slim_mem *out) {
    memset(out, 0, sizeof *out);
    ent *t = NULL; size_t n = 0, cap = 0;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *s = de->d_name;
        if (*s < '1' || *s > '9') continue;
        char *e; long v = strtol(s, &e, 10);
        if (*e) continue;
        pid_t pp = ppid_of((pid_t)v);
        if (pp < 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 512;
            ent *nt = realloc(t, nc * sizeof *nt);
            if (!nt) break;
            t = nt; cap = nc;
        }
        t[n].pid = (pid_t)v; t[n].ppid = pp; t[n].in = 0;
        for (int r = 0; r < nroots; r++) if (roots[r] == (pid_t)v) t[n].in = 2;   /* 2 = a root */
        n++;
    }
    closedir(d);
    /* Mark descendants: a pass that marks nothing new ends the walk. */
    for (int grew = 1; grew;) {
        grew = 0;
        for (size_t i = 0; i < n; i++) {
            if (t[i].in) continue;
            int hit = 0;
            for (size_t j = 0; !hit && j < n; j++) hit = t[j].in && t[j].pid == t[i].ppid;
            if (hit) { t[i].in = 1; grew = 1; }
        }
    }
    funion u = {0};
    int found = 0;
    slim_mem m;
    /* Roots first (in the order given), then their descendants. */
    for (int pass = 2; pass >= 1; pass--)
        for (size_t i = 0; i < n; i++) {
            if (t[i].in != pass || proc_mem(t[i].pid, &m, &u) != 0) continue;
            if (pass == 2) found = 1;
            if (cb) cb(t[i].pid, &m, ctx);
            add(out, &m);
        }
    out->file_union_kb = u.pages * (sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) / 1024 : 4);
    out->iso_kb = out->anon_kb + out->shmem_kb + out->file_union_kb;
    funion_free(&u);
    free(t);
    return found ? 0 : -1;
}

int slim_mem_tree_each(pid_t root, void (*cb)(pid_t, const slim_mem *, void *), void *ctx, slim_mem *out) {
    return slim_mem_trees(&root, 1, cb, ctx, out);
}

int slim_mem_tree(pid_t root, slim_mem *out) { return slim_mem_trees(&root, 1, NULL, NULL, out); }
