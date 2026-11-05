/* slim-cdp: minimal DevTools-protocol client for the benchmark (C11 + POSIX).
 *   slim-cdp [-t SECS] [-m MAXLEN] WS_URL STEP...
 * STEP is one of
 *   "Domain.method {params}"   send a command, print its reply (one line)
 *   "S:Domain.method {params}" same, on the session in $sessionId (flat mode)
 *   "wait Domain.event"        wait for an event (any session)
 *   "sleep MS"
 * "$name" in a step is replaced by the latest "name":"value" string (or
 * "name":number) seen in a reply (e.g. $targetId, $sessionId, $windowId). Replies are printed
 * truncated to MAXLEN bytes (default 4096, 0 = whole). Exit 1 if a command
 * answers with an error or times out. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fd = -1, timeout_s = 30;
static size_t maxlen = 4096;
static char *msg; static size_t msg_len, msg_cap;   /* last received message */
#define KEEP 16
static char *kept[KEEP]; static int nkept;             /* recent replies, for $name */

static void die(const char *m) { fprintf(stderr, "slim-cdp: %s\n", m); exit(1); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void wr(const void *b, size_t n) {
    const char *p = b;
    while (n) { ssize_t r = write(fd, p, n); if (r < 0) { if (errno == EINTR) continue; die("write failed"); } p += r; n -= (size_t)r; }
}

/* Read exactly n bytes, failing at the deadline. */
static void rd(void *b, size_t n, double deadline) {
    char *p = b;
    while (n) {
        int ms = (int)((deadline - now()) * 1000);
        if (ms <= 0) die("timeout");
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, ms) <= 0) continue;
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; die("read failed"); }
        if (r == 0) die("connection closed");
        p += r; n -= (size_t)r;
    }
}

static void send_frame(int op, const char *data, size_t n) {
    unsigned char h[14]; size_t k = 0;
    h[k++] = (unsigned char)(0x80 | op);
    if (n < 126) h[k++] = (unsigned char)(0x80 | n);
    else if (n < 65536) { h[k++] = 0x80 | 126; h[k++] = (unsigned char)(n >> 8); h[k++] = (unsigned char)n; }
    else { h[k++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[k++] = (unsigned char)((unsigned long long)n >> (8 * i)); }
    unsigned char mk[4]; for (int i = 0; i < 4; i++) mk[i] = (unsigned char)rand();
    memcpy(h + k, mk, 4); k += 4;
    wr(h, k);
    char buf[4096];
    for (size_t o = 0; o < n;) {
        size_t c = n - o < sizeof buf ? n - o : sizeof buf;
        for (size_t i = 0; i < c; i++) buf[i] = (char)(data[o + i] ^ mk[(o + i) & 3]);
        wr(buf, c); o += c;
    }
}

/* Next complete text message into msg (NUL-terminated). */
static void recv_msg(double deadline) {
    msg_len = 0;
    for (;;) {
        unsigned char h[2]; rd(h, 2, deadline);
        int op = h[0] & 15, fin = h[0] >> 7;
        unsigned long long n = h[1] & 127;
        if (n == 126) { unsigned char e[2]; rd(e, 2, deadline); n = (unsigned)e[0] << 8 | e[1]; }
        else if (n == 127) { unsigned char e[8]; rd(e, 8, deadline); n = 0; for (int i = 0; i < 8; i++) n = n << 8 | e[i]; }
        unsigned char mk[4] = {0}; int masked = h[1] >> 7;
        if (masked) rd(mk, 4, deadline);
        if (msg_len + n + 1 > msg_cap) {
            msg_cap = (msg_len + n + 1) * 2;
            if (!(msg = realloc(msg, msg_cap))) die("out of memory");
        }
        rd(msg + msg_len, (size_t)n, deadline);
        if (masked) for (unsigned long long i = 0; i < n; i++) msg[msg_len + i] ^= (char)mk[i & 3];
        if (op == 9) { send_frame(10, msg + msg_len, (size_t)n); continue; }   /* ping */
        if (op == 8) die("connection closed by peer");
        if (op == 10) continue;
        msg_len += (size_t)n;
        if (fin) { msg[msg_len] = 0; return; }
    }
}

static void ws_connect(const char *url) {
    if (strncmp(url, "ws://", 5)) die("WS_URL must start with ws://");
    char host[256], port[16] = "80"; const char *p = url + 5, *path = strchr(p, '/');
    if (!path) path = "/";
    size_t hl = (size_t)(path - p); if (hl >= sizeof host) die("bad url");
    memcpy(host, p, hl); host[hl] = 0;
    char *c = strchr(host, ':'); if (c) { *c = 0; snprintf(port, sizeof port, "%.15s", c + 1); }
    struct addrinfo hints = {0}, *ai; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai)) die("cannot resolve host");
    fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) die("cannot connect");
    freeaddrinfo(ai);
    char req[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: c2xpbWl1bS1iZW5jaC1rZXk=\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n", path, host, port);
    wr(req, (size_t)n);
    char resp[2048]; size_t k = 0; double dl = now() + timeout_s;
    while (k < 4 || memcmp(resp + k - 4, "\r\n\r\n", 4)) { if (k + 1 >= sizeof resp) die("bad handshake"); rd(resp + k, 1, dl); k++; }
    resp[k] = 0;
    if (!strstr(resp, " 101 ")) die("websocket upgrade refused");
}

/* Latest "name":"value" (string, inserted without quotes) or "name":123
 * (number) among kept replies. */
static const char *lookup(const char *name, size_t nl, size_t *vl) {
    char key[128]; if (nl + 3 > sizeof key) return NULL;
    key[0] = '"'; memcpy(key + 1, name, nl); memcpy(key + 1 + nl, "\":", 3); key[nl + 3] = 0;
    for (int i = nkept - 1; i >= 0; i--) {
        const char *s = strstr(kept[i], key);
        if (!s) continue;
        s += nl + 3;
        if (*s == '"') {
            const char *e = strchr(++s, '"');
            if (!e) continue;
            *vl = (size_t)(e - s); return s;
        }
        size_t n = 0; while (s[n] == '-' || s[n] == '.' || (s[n] >= '0' && s[n] <= '9')) n++;
        if (!n) continue;
        *vl = n; return s;
    }
    return NULL;
}

static char *subst(const char *in) {
    size_t cap = strlen(in) * 2 + 256, o = 0; char *out = malloc(cap);
    if (!out) die("out of memory");
    for (const char *p = in; *p;) {
        if (*p == '$' && (p[1] == '_' || (p[1] | 32) >= 'a') ) {
            const char *s = p + 1; size_t nl = 0;
            while (s[nl] == '_' || (s[nl] >= 'a' && s[nl] <= 'z') || (s[nl] >= 'A' && s[nl] <= 'Z') || (s[nl] >= '0' && s[nl] <= '9')) nl++;
            size_t vl; const char *v = nl ? lookup(s, nl, &vl) : NULL;
            if (!v) { fprintf(stderr, "slim-cdp: no value for $%.*s\n", (int)nl, s); exit(1); }
            if (o + vl + 1 > cap) { cap = (o + vl) * 2 + 64; if (!(out = realloc(out, cap))) die("out of memory"); }
            memcpy(out + o, v, vl); o += vl; p = s + nl; continue;
        }
        if (o + 2 > cap) { cap *= 2; if (!(out = realloc(out, cap))) die("out of memory"); }
        out[o++] = *p++;
    }
    out[o] = 0; return out;
}

static void keep(void) {
    size_t n = msg_len < 8192 ? msg_len : 8192;
    char *c = malloc(n + 1); if (!c) die("out of memory");
    memcpy(c, msg, n); c[n] = 0;
    if (nkept == KEEP) { free(kept[0]); memmove(kept, kept + 1, (KEEP - 1) * sizeof *kept); nkept--; }
    kept[nkept++] = c;
}

int main(int argc, char **argv) {
    int i = 1, failed = 0, id = 0;
    for (; i < argc && argv[i][0] == '-'; i += 2) {
        if (i + 1 >= argc) break;
        if (!strcmp(argv[i], "-t")) timeout_s = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-m")) maxlen = (size_t)atol(argv[i + 1]);
        else break;
    }
    if (i >= argc) { fprintf(stderr, "usage: slim-cdp [-t SECS] [-m MAXLEN] WS_URL STEP...\n"); return 2; }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    ws_connect(argv[i++]);
    for (; i < argc; i++) {
        char *step = subst(argv[i]);
        double dl = now() + timeout_s;
        if (!strncmp(step, "sleep ", 6)) { struct timespec t = {atol(step + 6) / 1000, atol(step + 6) % 1000 * 1000000L}; nanosleep(&t, NULL); free(step); continue; }
        if (!strncmp(step, "wait ", 5)) {
            char want[256]; snprintf(want, sizeof want, "\"method\":\"%.200s\"", step + 5);
            do recv_msg(dl); while (!strstr(msg, want));
            keep(); printf("%.*s\n", (int)(maxlen && msg_len > maxlen ? maxlen : msg_len), msg);
            free(step); continue;
        }
        int sess = !strncmp(step, "S:", 2);
        char *m = step + (sess ? 2 : 0), *sp = strchr(m, ' ');
        const char *params = "{}";
        if (sp) { *sp = 0; params = sp + 1; }
        size_t sl = 0; const char *sid = sess ? lookup("sessionId", 9, &sl) : NULL;
        if (sess && !sid) die("no $sessionId for S: step");
        size_t cap = strlen(m) + strlen(params) + sl + 96; char *out = malloc(cap);
        if (!out) die("out of memory");
        int n = snprintf(out, cap, "{\"id\":%d,\"method\":\"%s\",\"params\":%s%s%.*s%s}", ++id, m, params,
                         sess ? ",\"sessionId\":\"" : "", (int)sl, sid ? sid : "", sess ? "\"" : "");
        send_frame(1, out, (size_t)n); free(out);
        char want[32]; int wl = snprintf(want, sizeof want, "{\"id\":%d,", id);
        do recv_msg(dl); while (strncmp(msg, want, (size_t)wl));
        keep();
        if (!strncmp(msg + wl, "\"error\"", 7)) failed = 1;
        printf("%.*s\n", (int)(maxlen && msg_len > maxlen ? maxlen : msg_len), msg);
        fflush(stdout);
        free(step);
    }
    send_frame(8, "", 0);
    close(fd);
    return failed;
}
