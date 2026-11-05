/* slim-memstat: memory of a process tree.
 *   slim-memstat [--json] [-v] [--crashpad] PID...
 * Prints procs, pss, rss, uss (kB) for each PID and all its descendants, summed.
 * -v adds one line per process with its Chromium --type (and utility sub-type).
 * --crashpad also counts Chromium's crash handlers, which are reparented away
 * from the browser: the --crashpad-handler-pid=N named by a tree member, and the
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
static slim_mem total;
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

static void each(pid_t pid, const slim_mem *m, void *ctx) {
    (void)ctx;
    char k[128];
    kind_of(pid, k, sizeof k);
    if (!verbose) return;
    if (json)
        printf("{\"pid\":%d,\"type\":\"%s\",\"pss_kb\":%ld,\"rss_kb\":%ld,\"uss_kb\":%ld}\n",
               (int)pid, k, m->pss_kb, m->rss_kb, m->uss_kb);
    else
        printf("%8d %-48.48s pss %7ld rss %7ld uss %7ld kB\n", (int)pid, k, m->pss_kb, m->rss_kb,
               m->uss_kb);
}

static void add(const slim_mem *m) {
    total.procs += m->procs; total.pss_kb += m->pss_kb;
    total.rss_kb += m->rss_kb; total.uss_kb += m->uss_kb;
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

static void add_crashpad(void) {
    long pp0; long long st0;
    if (!crash_pid || stat_of(crash_pid, &pp0, &st0)) return;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        long pp; long long st; char buf[4096];
        if (pid != crash_pid) {
            if (stat_of(pid, &pp, &st) || pp != pp0 || llabs(st - st0) > 100) continue;
            if (cmdline(pid, buf, sizeof buf) <= 0 || !strstr(buf, "chrome_crashpad_handler")) continue;
        }
        slim_mem m;
        if (slim_mem_proc(pid, &m)) continue;
        each(pid, &m, NULL);
        add(&m);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    int crashpad = 0, npids = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--crashpad")) crashpad = 1;
        else if (argv[i][0] >= '1' && argv[i][0] <= '9') {
            slim_mem m;
            if (slim_mem_tree_each((pid_t)atoi(argv[i]), each, NULL, &m) != 0) {
                fprintf(stderr, "slim-memstat: no such process %s\n", argv[i]);
                return 1;
            }
            add(&m); npids++;
        } else { npids = 0; break; }
    }
    if (!npids) { fprintf(stderr, "usage: slim-memstat [--json] [-v] [--crashpad] PID...\n"); return 2; }
    if (crashpad) add_crashpad();
    if (json)
        printf("{\"procs\":%d,\"pss_kb\":%ld,\"rss_kb\":%ld,\"uss_kb\":%ld}\n", total.procs,
               total.pss_kb, total.rss_kb, total.uss_kb);
    else
        printf("procs %d pss %ld kB rss %ld kB uss %ld kB\n", total.procs, total.pss_kb, total.rss_kb,
               total.uss_kb);
    return 0;
}
