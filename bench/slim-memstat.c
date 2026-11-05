/* slim-memstat: memory of a process tree.
 *   slim-memstat [--json] [-v] [--crashpad] PID...
 * Prints procs and pss/rss/uss/anon/shmem/file/file_union/iso/swap (kB) for the
 * PIDs and all their descendants, as one set (src/mem.c; iso = anon + shmem +
 * union of resident file pages = the isolated footprint). -v adds one line per
 * process with its Chromium --type (and utility sub-type). --crashpad also
 * counts Chromium's crash handlers, which are reparented away from the
 * browser: the --crashpad-handler-pid=N named by a tree member, and the
 * chrome_crashpad_handler started with it (its self-monitor). */
#define _POSIX_C_SOURCE 200809L
#include "../src/mem.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int json, verbose;
static pid_t crash_pid;

/* NUL-separated /proc/<pid>/cmdline with NULs turned into spaces (Chromium
 * rewrites its process title, so argv may already be one string). */
static ssize_t cmdline(pid_t pid, char *buf, size_t cap) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    for (ssize_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
    buf[n] = 0;
    return n;
}

static void word(const char *s, char *out, size_t cap) {
    size_t i = 0;
    while (s[i] && s[i] != ' ' && i + 1 < cap) { out[i] = s[i]; i++; }
    out[i] = 0;
}

static void kind_of(pid_t pid, char *out, size_t cap) {
    char buf[16384], t[64], u[64];
    snprintf(out, cap, "?");
    if (cmdline(pid, buf, sizeof buf) <= 0) return;
    const char *type = strstr(buf, " --type="), *sub = strstr(buf, " --utility-sub-type=");
    if (!type) {
        char exe[128]; word(buf, exe, sizeof exe);
        const char *b = strrchr(exe, '/');
        snprintf(out, cap, "%.120s", strstr(buf, "chrome_crashpad") ? "crashpad"
                                     : strstr(buf, "--user-data-dir") ? "browser" : (b ? b + 1 : exe));
        return;
    }
    word(type + 8, t, sizeof t);
    if (sub) { word(sub + 20, u, sizeof u); snprintf(out, cap, "%.40s:%.60s", t, u); }
    else snprintf(out, cap, "%.60s", t);
    if (!crash_pid) {
        const char *c = strstr(buf, "--crashpad-handler-pid=");
        if (c) crash_pid = (pid_t)atoi(c + 23);
    }
}

static void line(const char *pre, const slim_mem *m) {
    if (json)
        printf("{%s\"procs\":%d,\"pss_kb\":%ld,\"rss_kb\":%ld,\"uss_kb\":%ld,\"anon_kb\":%ld,\"shmem_kb\":%ld,"
               "\"file_kb\":%ld,\"file_union_kb\":%ld,\"iso_kb\":%ld,\"swap_kb\":%ld}\n", pre, m->procs, m->pss_kb,
               m->rss_kb, m->uss_kb, m->anon_kb, m->shmem_kb, m->file_kb, m->file_union_kb, m->iso_kb, m->swap_kb);
    else
        printf("%sprocs %d iso %ld pss %ld rss %ld uss %ld anon %ld shmem %ld file %ld file_union %ld swap %ld kB\n",
               pre, m->procs, m->iso_kb, m->pss_kb, m->rss_kb, m->uss_kb, m->anon_kb, m->shmem_kb, m->file_kb,
               m->file_union_kb, m->swap_kb);
}

static void each(pid_t pid, const slim_mem *m, void *ctx) {
    char k[128], pre[200];
    kind_of(pid, k, sizeof k);
    if (!verbose || !ctx) return;
    if (json) snprintf(pre, sizeof pre, "\"pid\":%d,\"type\":\"%s\",", (int)pid, k);
    else snprintf(pre, sizeof pre, "%8d %-40.40s ", (int)pid, k);
    line(pre, m);
}

/* Field 4 (ppid) and 22 (starttime) of /proc/<pid>/stat. */
static int stat_of(pid_t pid, long *ppid, long long *start) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *r = strrchr(buf, ')');
    if (!r) return -1;
    char *s = r + 2; int field = 3;
    while (*s && field < 22) {
        if (field == 4) *ppid = strtol(s, NULL, 10);
        s = strchr(s, ' '); if (!s) return -1; s++; field++;
    }
    *start = strtoll(s, NULL, 10);
    return 0;
}

/* Crash handler pids: crash_pid and its self-monitor (same parent, started
 * within a second, also chrome_crashpad_handler). */
static int find_crashpad(pid_t *out, int cap) {
    long pp0; long long st0; int n = 0;
    if (!crash_pid || stat_of(crash_pid, &pp0, &st0)) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < cap) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        long pp; long long st; char buf[4096];
        if (pid != crash_pid) {
            if (stat_of(pid, &pp, &st) || pp != pp0 || llabs(st - st0) > 100) continue;
            if (cmdline(pid, buf, sizeof buf) <= 0 || !strstr(buf, "chrome_crashpad_handler")) continue;
        }
        out[n++] = pid;
    }
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    int crashpad = 0, n = 0;
    pid_t roots[64];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--crashpad")) crashpad = 1;
        else if (argv[i][0] >= '1' && argv[i][0] <= '9' && n < 56) roots[n++] = (pid_t)atoi(argv[i]);
        else { n = 0; break; }
    }
    if (!n) { fprintf(stderr, "usage: slim-memstat [--json] [-v] [--crashpad] PID...\n"); return 2; }
    slim_mem m;
    if (crashpad) {   /* first pass only learns the crash handler pid */
        if (slim_mem_trees(roots, n, each, NULL, &m) != 0) goto gone;
        n += find_crashpad(roots + n, 8);
    }
    if (slim_mem_trees(roots, n, each, (void *)1, &m) != 0) goto gone;
    line("", &m);
    return 0;
gone:
    fprintf(stderr, "slim-memstat: no such process\n");
    return 1;
}
