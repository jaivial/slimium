/* Placeholder for src/mem.c (memory track). Built ONLY while src/mem.c does not exist (see Makefile).
 * Sums /proc/<pid>/smaps_rollup over pid and all descendants. */
#include "mem.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long field(const char *buf, const char *name) {
    const char *p = strstr(buf, name);
    return p ? atol(p + strlen(name)) : 0;
}

int slim_mem_tree(pid_t root, slim_mem *out) {
    memset(out, 0, sizeof *out);
    pid_t *pids = NULL, *ppids = NULL; size_t n = 0, cap = 0;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        char *rp = strrchr(buf, ')');
        int ppid = 0; char st;
        if (!rp || sscanf(rp + 1, " %c %d", &st, &ppid) != 2) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            pid_t *a = realloc(pids, cap * sizeof *a), *b = realloc(ppids, cap * sizeof *b);
            if (!a || !b) { free(a ? a : pids); free(b ? b : ppids); closedir(d); return -1; }
            pids = a; ppids = b;
        }
        pids[n] = (pid_t)pid; ppids[n] = (pid_t)ppid; n++;
    }
    closedir(d);
    /* breadth-first walk from root */
    pid_t *q = malloc((n + 1) * sizeof *q);
    if (!q) { free(pids); free(ppids); return -1; }
    size_t qh = 0, qt = 0; int found = 0;
    for (size_t i = 0; i < n; i++) if (pids[i] == root) found = 1;
    if (!found) { free(q); free(pids); free(ppids); return -1; }
    q[qt++] = root;
    while (qh < qt) {
        pid_t p = q[qh++];
        char path[64], buf[4096];
        snprintf(path, sizeof path, "/proc/%d/smaps_rollup", (int)p);
        FILE *f = fopen(path, "r");
        if (f) {
            size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
            out->procs++;
            out->rss_kb += field(buf, "\nRss:");
            out->pss_kb += field(buf, "\nPss:");
            out->uss_kb += field(buf, "\nPrivate_Clean:") + field(buf, "\nPrivate_Dirty:");
        }
        for (size_t i = 0; i < n && qt < n + 1; i++) if (ppids[i] == p && pids[i] != p) q[qt++] = pids[i];
    }
    free(q); free(pids); free(ppids);
    return 0;
}
