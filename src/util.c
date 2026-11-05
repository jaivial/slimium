#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void oom(void) {
    static const char m[] = "slimium: out of memory\n";
    ssize_t r = write(2, m, sizeof m - 1);
    (void)r;
    _exit(1);
}

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) oom(); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n); p[n] = 0; return p;
}

void sb_reserve(sbuf *b, size_t extra) {
    if (extra > (size_t)-1 / 2 - b->n) oom();
    size_t need = b->n + extra + 1;
    if (need <= b->cap) return;
    size_t c = b->cap ? b->cap : 64;
    while (c < need) c *= 2;
    b->p = xrealloc(b->p, c); b->cap = c;
}
void sb_add(sbuf *b, const void *s, size_t n) {
    sb_reserve(b, n);
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n; b->p[b->n] = 0;
}
void sb_puts(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }
void sb_putc(sbuf *b, char c) { sb_add(b, &c, 1); }
void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[256];
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) { sb_add(b, tmp, (size_t)k); return; }
    sb_reserve(b, (size_t)k);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}
void sb_jstr(sbuf *b, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    sb_reserve(b, n + 2);
    sb_putc(b, '"');
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        sb_add(b, s + run, i - run); run = i + 1;
        char e[7] = {'\\', 0};
        switch (c) {
        case '"': e[1] = '"'; break;
        case '\\': e[1] = '\\'; break;
        case '\n': e[1] = 'n'; break;
        case '\r': e[1] = 'r'; break;
        case '\t': e[1] = 't'; break;
        case '\b': e[1] = 'b'; break;
        case '\f': e[1] = 'f'; break;
        default: e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hx[c >> 4]; e[5] = hx[c & 15]; break;
        }
        sb_puts(b, e);
    }
    sb_add(b, s + run, n - run);
    sb_putc(b, '"');
}
void sb_jstrz(sbuf *b, const char *s) { if (s) sb_jstr(b, s, strlen(s)); else sb_puts(b, "null"); }
void sb_reset(sbuf *b) { b->n = 0; if (b->p) b->p[0] = 0; }
void sb_free(sbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
const char *sb_cstr(sbuf *b) { return b->p ? b->p : ""; }

static char errbuf[512];
int slim_seterr(const char *fmt, ...) {
    char tmp[sizeof errbuf]; /* args may alias errbuf */
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    memcpy(errbuf, tmp, sizeof errbuf);
    for (char *p = errbuf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    return -1;
}
const char *slim_errmsg(void) { return errbuf[0] ? errbuf : "unknown error"; }

long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

int read_fd_all(int fd, char **data, size_t *len, size_t max) {
    sbuf b = {0};
    char tmp[16384];
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r < 0) { if (errno == EINTR) continue; sb_free(&b); return slim_seterr("read: %s", strerror(errno)); }
        if (r == 0) break;
        if (b.n + (size_t)r > max) { sb_free(&b); return slim_seterr("input too large"); }
        sb_add(&b, tmp, (size_t)r);
    }
    if (!b.p) sb_add(&b, "", 0);
    *data = b.p; if (len) *len = b.n;
    return 0;
}
int read_file(const char *path, char **data, size_t *len, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return slim_seterr("%s: %s", path, strerror(errno));
    int r = read_fd_all(fd, data, len, max);
    close(fd);
    return r;
}
int write_file(const char *path, const void *data, size_t len, int atomic) {
    char tmp[4096];
    const char *dst = path;
    if (atomic) {
        if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof tmp)
            return slim_seterr("path too long");
        dst = tmp;
    }
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, atomic ? 0600 : 0644);
    if (fd < 0) return slim_seterr("%s: %s", dst, strerror(errno));
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno; close(fd); if (atomic) unlink(dst);
            return slim_seterr("%s: %s", dst, strerror(e));
        }
        p += w; len -= (size_t)w;
    }
    if (close(fd)) { if (atomic) unlink(dst); return slim_seterr("%s: %s", dst, strerror(errno)); }
    if (atomic && rename(dst, path)) { int e = errno; unlink(dst); return slim_seterr("%s: %s", path, strerror(e)); }
    return 0;
}
int mkdir_p(const char *path, int mode) {
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return slim_seterr("bad path");
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == 0) {
            char c = buf[i]; buf[i] = 0;
            if (mkdir(buf, (mode_t)mode) && errno != EEXIST) return slim_seterr("mkdir %s: %s", buf, strerror(errno));
            buf[i] = c;
        }
    }
    return 0;
}
const char *path_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
char *find_in_path(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) == 0 ? xstrdup(name) : NULL;
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    sbuf b = {0};
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        sb_reset(&b);
        if (l) sb_add(&b, path, l); else sb_putc(&b, '.');
        sb_putc(&b, '/'); sb_puts(&b, name);
        struct stat st;
        if (stat(b.p, &st) == 0 && S_ISREG(st.st_mode) && access(b.p, X_OK) == 0) return b.p;
        if (!e) break;
        path = e + 1;
    }
    sb_free(&b);
    return NULL;
}
int parse_int(const char *s, long *out) {
    if (!s || !*s) return -1;
    char *e; errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || *e) return -1;
    *out = v; return 0;
}
int parse_double(const char *s, double *out) {
    if (!s || !*s) return -1;
    char *e; errno = 0;
    double v = strtod(s, &e);
    if (errno || *e) return -1;
    *out = v; return 0;
}
