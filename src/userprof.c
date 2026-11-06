/* Persistent named user-profile registry. Metadata never contains browser storage values. */
#include "userprof.h"
#include "json.h"
#include "state.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define META_MAX (1u << 20)

int userprof_valid_name(const char *name) {
    /* state_valid_name is ASCII-only, bounded, and rejects leading dots/slashes. */
    return state_valid_name(name) && strcmp(name, ".") && strcmp(name, "..");
}

static long long wall_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) return (long long)time(NULL) * 1000;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int root_path(char *buf, size_t cap, int create) {
    const char *root = getenv("SLIM_PROFILES_ROOT");
    int n;
    if (root && *root) {
        if (root[0] != '/') return slim_seterr("SLIM_PROFILES_ROOT must be an absolute path");
        n = snprintf(buf, cap, "%s", root);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') return slim_seterr("HOME is not set; set SLIM_PROFILES_ROOT");
        n = snprintf(buf, cap, "%s/.local/share/slimium/profiles", home);
    }
    if (n < 0 || (size_t)n >= cap) return slim_seterr("profiles root path too long");
    if (!strcmp(buf, "/")) return slim_seterr("refusing to use / as profiles root");
    if (create && mkdir_p(buf, 0700)) return -1;
    struct stat st;
    if (lstat(buf, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return slim_seterr("profiles root is not a real directory: %s", buf);
        if (create && chmod(buf, 0700)) return slim_seterr("chmod %s: %s", buf, strerror(errno));
        char resolved[PATH_MAX];
        if (!realpath(buf, resolved)) return slim_seterr("resolve %s: %s", buf, strerror(errno));
        n = snprintf(buf, cap, "%s", resolved);
        if (n < 0 || (size_t)n >= cap) return slim_seterr("profiles root path too long");
    } else if (create || errno != ENOENT) return slim_seterr("%s: %s", buf, strerror(errno));
    return 0;
}

static int path3(char *buf, size_t cap, const char *root, const char *a, const char *b) {
    int n = b ? snprintf(buf, cap, "%s/%s/%s", root, a, b) : snprintf(buf, cap, "%s/%s", root, a);
    if (n < 0 || (size_t)n >= cap) return slim_seterr("profile path too long");
    return 0;
}

static int lock_registry(const char *root, int exclusive, int *fd_out) {
    char path[PATH_MAX];
    if (path3(path, sizeof path, root, ".lock", NULL)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return slim_seterr("open profile registry lock: %s", strerror(errno));
    if (fchmod(fd, 0600) || flock(fd, exclusive ? LOCK_EX : LOCK_SH)) {
        int e = errno; close(fd); return slim_seterr("lock profile registry: %s", strerror(e));
    }
    *fd_out = fd;
    return 0;
}

static void unlock_registry(int fd) {
    flock(fd, LOCK_UN);
    close(fd);
}

void userprof_free(userprof_t *p) {
    free(p->name); free(p->ua); free(p->locale); free(p->pages); free(p->current_url);
    memset(p, 0, sizeof *p);
}

static int load_at(const char *root, const char *name, userprof_t *p) {
    memset(p, 0, sizeof *p);
    if (!userprof_valid_name(name)) return slim_seterr("invalid user profile name '%s'", name ? name : "");
    char dir[PATH_MAX], path[PATH_MAX];
    if (path3(dir, sizeof dir, root, name, NULL) || path3(path, sizeof path, root, name, "profile.json")) return -1;
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) return slim_seterr("no user profile '%s'", name);
    char *doc; size_t len;
    if (read_file(path, &doc, &len, META_MAX)) return -1;
    jv d, v;
    if (json_parse(doc, len, &d) || d.t != J_OBJ || json_get_num(d, "version", 0) != 1) {
        free(doc); return slim_seterr("corrupt metadata for user profile '%s'", name);
    }
    char *stored = json_get_str(d, "name");
    if (!stored || strcmp(stored, name)) {
        free(stored); free(doc); return slim_seterr("metadata name mismatch for user profile '%s'", name);
    }
    free(stored);
    p->name = xstrdup(name);
    p->ua = json_get_str(d, "ua");
    p->locale = json_get_str(d, "locale");
    p->created_ms = (long long)json_get_num(d, "created_ms", 0);
    p->updated_ms = (long long)json_get_num(d, "updated_ms", 0);
    p->saved_ms = (long long)json_get_num(d, "saved_ms", 0);
    p->current_index = (int)json_get_num(d, "current_index", 0);
    p->current_url = json_get_str(d, "current_url");
    if (json_get(d, "pages", &v)) {
        if (v.t != J_ARR || json_len(v) > 256) {
            userprof_free(p); free(doc); return slim_seterr("invalid saved pages for user profile '%s'", name);
        }
        p->pages = xstrndup(v.p, v.n);
    } else p->pages = xstrdup("[]");
    free(doc);
    return 0;
}

int userprof_load(const char *name, userprof_t *p) {
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int fd;
    if (lock_registry(root, 0, &fd)) return -1;
    int r = load_at(root, name, p);
    unlock_registry(fd);
    return r;
}

static int write_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        data += n; len -= (size_t)n;
    }
    return 0;
}

static int atomic_write(const char *path, const char *data, size_t len) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) return slim_seterr("metadata path too long");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return slim_seterr("write %s: %s", tmp, strerror(errno));
    int r = 0;
    if (write_all(fd, data, len) || fsync(fd) || close(fd)) r = -1;
    if (!r && rename(tmp, path)) r = -1;
    if (r) {
        int e = errno; close(fd); unlink(tmp); return slim_seterr("atomic metadata update %s: %s", path, strerror(e));
    }
    return 0;
}

static int write_meta_at(const char *root, const userprof_t *p) {
    char path[PATH_MAX];
    if (path3(path, sizeof path, root, p->name, "profile.json")) return -1;
    sbuf b = {0};
    sb_printf(&b, "{\"version\":1,\"name\":");
    sb_jstrz(&b, p->name);
    sb_printf(&b, ",\"created_ms\":%lld,\"updated_ms\":%lld", p->created_ms, p->updated_ms);
    if (p->ua && *p->ua) { sb_puts(&b, ",\"ua\":"); sb_jstrz(&b, p->ua); }
    if (p->locale && *p->locale) { sb_puts(&b, ",\"locale\":"); sb_jstrz(&b, p->locale); }
    if (p->saved_ms) {
        sb_printf(&b, ",\"saved_ms\":%lld,\"current_index\":%d,\"current_url\":", p->saved_ms, p->current_index);
        sb_jstrz(&b, p->current_url);
        sb_puts(&b, ",\"pages\":"); sb_puts(&b, p->pages ? p->pages : "[]");
    }
    sb_puts(&b, "}\n");
    int r = atomic_write(path, b.p, b.n);
    sb_free(&b);
    return r;
}

int userprof_create(const char *name, const char *ua, const char *locale) {
    if (!userprof_valid_name(name)) return slim_seterr("invalid user profile name '%s' (use 1-64 ASCII letters, digits, ._-; no leading dot)", name ? name : "");
    if ((ua && strlen(ua) > 2048) || (locale && strlen(locale) > 128)) return slim_seterr("user agent or locale is too long");
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 1, &lock)) return -1;
    char dir[PATH_MAX], data[PATH_MAX];
    if (path3(dir, sizeof dir, root, name, NULL) || path3(data, sizeof data, root, name, "data")) { unlock_registry(lock); return -1; }
    if (mkdir(dir, 0700)) {
        int e = errno; unlock_registry(lock);
        return e == EEXIST ? slim_seterr("user profile '%s' already exists", name) : slim_seterr("create user profile '%s': %s", name, strerror(e));
    }
    userprof_t p; memset(&p, 0, sizeof p);
    p.name = (char *)name; p.ua = (char *)ua; p.locale = (char *)locale;
    p.created_ms = p.updated_ms = wall_ms(); p.pages = "[]";
    int r = mkdir(data, 0700) || write_meta_at(root, &p);
    if (r) { rmdir(data); rmdir(dir); }
    unlock_registry(lock);
    return r;
}

int userprof_default(char **name) {
    *name = NULL;
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 0)) return -1;
    char path[PATH_MAX];
    if (path3(path, sizeof path, root, ".current", NULL)) return -1;
    if (access(path, F_OK)) return errno == ENOENT ? 1 : slim_seterr("%s: %s", path, strerror(errno));
    char *doc; size_t len;
    if (read_file(path, &doc, &len, 128)) return -1;
    while (len && (doc[len - 1] == '\n' || doc[len - 1] == '\r' || doc[len - 1] == ' ' || doc[len - 1] == '\t')) doc[--len] = 0;
    if (!userprof_valid_name(doc)) { free(doc); return slim_seterr("corrupt default user profile selection"); }
    userprof_t p;
    if (userprof_load(doc, &p)) { free(doc); return -1; }
    userprof_free(&p);
    *name = xstrdup(doc);
    free(doc);
    return 0;
}

int userprof_set_default(const char *name) {
    userprof_t p;
    if (userprof_load(name, &p)) return -1;
    userprof_free(&p);
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 1, &lock)) return -1;
    char path[PATH_MAX];
    int r = path3(path, sizeof path, root, ".current", NULL);
    if (!r) {
        sbuf b = {0}; sb_printf(&b, "%s\n", name);
        r = atomic_write(path, b.p, b.n); sb_free(&b);
    }
    unlock_registry(lock);
    return r;
}

int userprof_list(userprof_t **items, size_t *count) {
    *items = NULL; *count = 0;
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 0, &lock)) return -1;
    DIR *dir = opendir(root);
    if (!dir) { int e = errno; unlock_registry(lock); return slim_seterr("list profiles: %s", strerror(e)); }
    size_t cap = 0;
    struct dirent *ent;
    int r = 0;
    while (!r && (ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;
        userprof_t p;
        if (load_at(root, ent->d_name, &p)) {
            /* A non-profile entry is not a registry item; malformed NAME dirs are errors. */
            char marker[PATH_MAX];
            if (!path3(marker, sizeof marker, root, ent->d_name, "profile.json") && !access(marker, F_OK)) { r = -1; break; }
            continue;
        }
        if (*count == cap) { cap = cap ? cap * 2 : 8; *items = xrealloc(*items, cap * sizeof **items); }
        (*items)[(*count)++] = p;
    }
    closedir(dir); unlock_registry(lock);
    if (r) {
        for (size_t i = 0; i < *count; i++) userprof_free(&(*items)[i]);
        free(*items); *items = NULL; *count = 0;
        return -1;
    }
    return 0;
}

static int cmp_profiles(const void *a, const void *b) {
    const userprof_t *pa = a, *pb = b;
    return strcmp(pa->name, pb->name);
}

/* Sort without exposing qsort in the command module's read loop. */
void userprof_sort(userprof_t *items, size_t count) { qsort(items, count, sizeof *items, cmp_profiles); }

static int proc_uses_dir(const char *data_dir) {
    char needle[PATH_MAX + 32];
    int n = snprintf(needle, sizeof needle, "--user-data-dir=%s", data_dir);
    if (n < 0 || (size_t)n >= sizeof needle) return 0;
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *ent;
    int used = 0;
    while (!used && (ent = readdir(proc))) {
        long pid;
        if (parse_int(ent->d_name, &pid) || pid <= 0) continue;
        char path[64], *cmd; size_t len;
        snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
        if (read_file(path, &cmd, &len, META_MAX)) continue;
        for (size_t i = 0; i < len; i++) if (!cmd[i]) cmd[i] = ' ';
        size_t nl = strlen(needle);
        for (char *s = cmd; (s = strstr(s, needle)); s += nl) {
            if ((s == cmd || s[-1] == ' ') && (s[nl] == ' ' || s[nl] == 0)) { used = 1; break; }
        }
        free(cmd);
    }
    closedir(proc);
    return used;
}

int userprof_check_idle(const char *name) {
    char dir[PATH_MAX];
    if (userprof_data_dir(name, dir, sizeof dir)) return -1;
    if (proc_uses_dir(dir)) return slim_seterr("user profile '%s' is already in use by another browser", name);
    return 0;
}

int userprof_rename(const char *old_name, const char *new_name) {
    if (!userprof_valid_name(old_name) || !userprof_valid_name(new_name))
        return slim_seterr("invalid old or new user profile name");
    if (!strcmp(old_name, new_name)) return slim_seterr("old and new user profile names are the same");
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 1, &lock)) return -1;
    userprof_t p;
    int r = load_at(root, old_name, &p);
    char old_dir[PATH_MAX], new_dir[PATH_MAX], data_dir[PATH_MAX], current[PATH_MAX];
    if (!r) r = path3(old_dir, sizeof old_dir, root, old_name, NULL) ||
               path3(new_dir, sizeof new_dir, root, new_name, NULL) ||
               path3(data_dir, sizeof data_dir, root, old_name, "data") ||
               path3(current, sizeof current, root, ".current", NULL);
    struct stat st;
    if (!r && !lstat(new_dir, &st)) r = slim_seterr("user profile '%s' already exists", new_name);
    if (!r && proc_uses_dir(data_dir)) r = slim_seterr("cannot rename active user profile '%s'", old_name);
    if (!r && rename(old_dir, new_dir)) r = slim_seterr("rename user profile: %s", strerror(errno));
    if (!r) {
        free(p.name); p.name = xstrdup(new_name); p.updated_ms = wall_ms();
        r = write_meta_at(root, &p);
        if (r && rename(new_dir, old_dir)) slim_seterr("metadata update failed and rollback failed: %s", strerror(errno));
    }
    if (!r && !access(current, F_OK)) {
        char *selected = NULL; size_t len = 0;
        if (!read_file(current, &selected, &len, 128)) {
            while (len && (selected[len - 1] == '\n' || selected[len - 1] == '\r')) selected[--len] = 0;
            if (!strcmp(selected, old_name)) {
                sbuf b = {0}; sb_printf(&b, "%s\n", new_name);
                r = atomic_write(current, b.p, b.n); sb_free(&b);
            }
            free(selected);
        }
    }
    userprof_free(&p); unlock_registry(lock);
    return r;
}

static int remove_tree_at(int parent, const char *name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return slim_seterr("delete profile entry: %s", strerror(errno));
    if (!S_ISDIR(st.st_mode)) {
        if (unlinkat(parent, name, 0)) return slim_seterr("delete profile file: %s", strerror(errno));
        return 0;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return slim_seterr("open profile directory for deletion: %s", strerror(errno));
    DIR *dir = fdopendir(fd);
    if (!dir) { int e = errno; close(fd); return slim_seterr("read profile directory for deletion: %s", strerror(e)); }
    struct dirent *ent;
    int r = 0;
    errno = 0;
    while (!r && (ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        r = remove_tree_at(fd, ent->d_name);
        errno = 0;
    }
    int e = errno;
    closedir(dir);
    if (!r && e) r = slim_seterr("read profile directory for deletion: %s", strerror(e));
    if (!r && unlinkat(parent, name, AT_REMOVEDIR)) r = slim_seterr("remove profile directory: %s", strerror(errno));
    return r;
}

int userprof_delete(const char *name) {
    if (!userprof_valid_name(name)) return slim_seterr("invalid user profile name '%s'", name ? name : "");
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 1, &lock)) return -1;
    userprof_t p;
    int r = load_at(root, name, &p);
    userprof_free(&p);
    char dir[PATH_MAX], data[PATH_MAX], current[PATH_MAX];
    if (!r) r = path3(dir, sizeof dir, root, name, NULL) || path3(data, sizeof data, root, name, "data") ||
               path3(current, sizeof current, root, ".current", NULL);
    if (!r && proc_uses_dir(data)) r = slim_seterr("cannot delete active user profile '%s'", name);
    int rootfd = -1;
    if (!r) {
        rootfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (rootfd < 0) r = slim_seterr("open profiles root: %s", strerror(errno));
    }
    if (!r) r = remove_tree_at(rootfd, name);
    if (rootfd >= 0) close(rootfd);
    if (!r && !access(current, F_OK)) {
        char *selected = NULL; size_t len = 0;
        if (!read_file(current, &selected, &len, 128)) {
            while (len && (selected[len - 1] == '\n' || selected[len - 1] == '\r')) selected[--len] = 0;
            if (!strcmp(selected, name) && unlink(current)) r = slim_seterr("clear default user profile: %s", strerror(errno));
            free(selected);
        }
    }
    unlock_registry(lock);
    return r;
}

int userprof_data_dir(const char *name, char *buf, size_t cap) {
    userprof_t p;
    if (userprof_load(name, &p)) return -1;
    userprof_free(&p);
    char root[PATH_MAX], dir[PATH_MAX], data[PATH_MAX];
    if (root_path(root, sizeof root, 1) || path3(dir, sizeof dir, root, name, NULL) || path3(data, sizeof data, root, name, "data")) return -1;
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) return slim_seterr("invalid directory for user profile '%s'", name);
    if (chmod(dir, 0700) || mkdir_p(data, 0700) || chmod(data, 0700)) return slim_seterr("prepare user profile data: %s", strerror(errno));
    char resolved[PATH_MAX];
    if (!realpath(data, resolved)) return slim_seterr("resolve user profile data: %s", strerror(errno));
    int n = snprintf(buf, cap, "%s", resolved);
    if (n < 0 || (size_t)n >= cap) return slim_seterr("user profile data path too long");
    return 0;
}

int userprof_save_pages(const char *name, const char *pages_json, int current_index, const char *current_url) {
    jv pages;
    if (!pages_json || json_parse(pages_json, strlen(pages_json), &pages) || pages.t != J_ARR || json_len(pages) > 256)
        return slim_seterr("invalid tab snapshot");
    char root[PATH_MAX];
    if (root_path(root, sizeof root, 1)) return -1;
    int lock;
    if (lock_registry(root, 1, &lock)) return -1;
    userprof_t p;
    int r = load_at(root, name, &p);
    if (!r) {
        free(p.pages); p.pages = xstrdup(pages_json);
        free(p.current_url); p.current_url = current_url ? xstrdup(current_url) : NULL;
        p.current_index = current_index; p.saved_ms = wall_ms(); p.updated_ms = p.saved_ms;
        r = write_meta_at(root, &p);
    }
    userprof_free(&p); unlock_registry(lock);
    return r;
}
