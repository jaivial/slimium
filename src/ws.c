#include "ws.h"
#include "b64.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RBUF 65536
#define DEFAULT_MAX_MSG ((size_t)1 << 30)

/* ---------- SHA-1 (for Sec-WebSocket-Accept) ---------- */
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1_block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void slim_sha1(const unsigned char *data, size_t n, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha1_block(h, data + i);
    unsigned char blk[128] = {0};
    size_t r = n - i;
    memcpy(blk, data + i, r);
    blk[r] = 0x80;
    size_t tot = r + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) blk[tot - 1 - j] = (unsigned char)(bits >> (8 * j));
    sha1_block(h, blk);
    if (tot == 128) sha1_block(h, blk + 64);
    for (int j = 0; j < 5; j++) { out[4 * j] = (unsigned char)(h[j] >> 24); out[4 * j + 1] = (unsigned char)(h[j] >> 16); out[4 * j + 2] = (unsigned char)(h[j] >> 8); out[4 * j + 3] = (unsigned char)h[j]; }
}

/* ---------- TCP ---------- */
static int wait_fd(int fd, short ev, long long deadline) {
    for (;;) {
        long long left = deadline - now_ms();
        if (left < 0) left = 0;
        struct pollfd p = {fd, ev, 0};
        int r = poll(&p, 1, (int)(left > 0x7fffffff ? 0x7fffffff : left));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) return 0;
        return 1;
    }
}

int tcp_connect(const char *host, int port, int timeout_ms) {
    struct sockaddr_storage ss; socklen_t sl;
    memset(&ss, 0, sizeof ss);
    struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
    if (port <= 0 || port > 65535) return slim_seterr("bad port %d", port);
    if (!strcmp(host, "localhost") || inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
        if (!strcmp(host, "localhost")) a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a4->sin_family = AF_INET; a4->sin_port = htons((uint16_t)port); sl = sizeof *a4;
    } else if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6; a6->sin6_port = htons((uint16_t)port); sl = sizeof *a6;
    } else return slim_seterr("unsupported host '%s' (numeric loopback address expected)", host);
    int fd = socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return slim_seterr("socket: %s", strerror(errno));
    int r = connect(fd, (struct sockaddr *)&ss, sl);
    if (r < 0 && errno != EINPROGRESS) {
        int e = errno; close(fd);
        return slim_seterr("connect %s:%d: %s", host, port, strerror(e));
    }
    if (r < 0) {
        int w = wait_fd(fd, POLLOUT, now_ms() + timeout_ms);
        int err = 0; socklen_t el = sizeof err;
        if (w <= 0) { close(fd); return slim_seterr("connect %s:%d: timeout", host, port); }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return slim_seterr("connect %s:%d: %s", host, port, strerror(err)); }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int write_all(int fd, const void *buf, size_t n, long long deadline) {
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int r = wait_fd(fd, POLLOUT, deadline);
                if (r <= 0) return slim_seterr("send: %s", r ? strerror(errno) : "timeout");
                continue;
            }
            return slim_seterr("send: %s", strerror(errno));
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* read up to n bytes; 0 = EOF, -1 error/timeout */
static ssize_t read_some(int fd, void *buf, size_t n, long long deadline) {
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int w = wait_fd(fd, POLLIN, deadline);
            if (w == 0) { slim_seterr("read timeout"); return -1; }
            if (w < 0) { slim_seterr("poll: %s", strerror(errno)); return -1; }
            continue;
        }
        slim_seterr("recv: %s", strerror(errno));
        return -1;
    }
}

int http_get(const char *host, int port, const char *path, int timeout_ms, char **body, size_t *len) {
    long long dl = now_ms() + timeout_ms;
    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) return -1;
    sbuf rq = {0};
    sb_printf(&rq, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nAccept: application/json\r\n\r\n", path, host, port);
    if (write_all(fd, rq.p, rq.n, dl)) { sb_free(&rq); close(fd); return -1; }
    sb_free(&rq);
    sbuf b = {0};
    char tmp[8192];
    long clen = -1; size_t hdr = 0; int status = -1;
    for (;;) {
        ssize_t r = read_some(fd, tmp, sizeof tmp, dl);
        if (r < 0) { sb_free(&b); close(fd); return -1; }
        if (r == 0) break;
        if (b.n + (size_t)r > (16u << 20)) { sb_free(&b); close(fd); return slim_seterr("http response too large"); }
        sb_add(&b, tmp, (size_t)r);
        if (!hdr) {
            char *e = strstr(b.p, "\r\n\r\n");
            if (e) {
                hdr = (size_t)(e - b.p) + 4;
                if (strncmp(b.p, "HTTP/1.", 7) || b.n < 12) { sb_free(&b); close(fd); return slim_seterr("bad http response"); }
                status = atoi(b.p + 9);
                for (char *l = strstr(b.p, "\r\n"); l && l < e; l = strstr(l + 2, "\r\n")) {
                    if (!strncasecmp(l + 2, "content-length:", 15)) clen = atol(l + 17);
                    if (!strncasecmp(l + 2, "transfer-encoding:", 18) && strstr(l + 2, "chunked") && strstr(l + 2, "chunked") < strstr(l + 2, "\r\n")) {
                        sb_free(&b); close(fd); return slim_seterr("chunked http not supported");
                    }
                }
            }
        }
        if (hdr && clen >= 0 && b.n >= hdr + (size_t)clen) break;
    }
    close(fd);
    if (!hdr) { sb_free(&b); return slim_seterr("bad http response"); }
    size_t bl = b.n - hdr;
    if (clen >= 0 && (size_t)clen < bl) bl = (size_t)clen;
    memmove(b.p, b.p + hdr, bl); b.p[bl] = 0;
    *body = b.p; if (len) *len = bl;
    return status;
}

/* ---------- WebSocket ---------- */
int ws_parse_url(const char *url, char *host, size_t hostn, int *port, char **path) {
    if (strncmp(url, "ws://", 5)) return slim_seterr("only ws:// urls are supported: %s", url);
    const char *h = url + 5, *he, *pe;
    if (*h == '[') { he = strchr(h, ']'); if (!he) return slim_seterr("bad url"); h++; pe = he + 1; }
    else { he = h + strcspn(h, ":/"); pe = he; }
    size_t hl = (size_t)(he - h);
    if (hl == 0 || hl >= hostn) return slim_seterr("bad host in url");
    memcpy(host, h, hl); host[hl] = 0;
    *port = 80;
    if (*pe == ':') { *port = atoi(pe + 1); pe += 1 + strspn(pe + 1, "0123456789"); }
    *path = xstrdup(*pe == '/' ? pe : "/");
    return 0;
}

void ws_init_fd(ws_t *w, int fd) {
    memset(w, 0, sizeof *w);
    w->fd = fd;
    w->rcap = RBUF;
    w->rb = xmalloc(RBUF);
    w->max_msg = DEFAULT_MAX_MSG;
    w->rng = (unsigned)now_ms() ^ ((unsigned)getpid() << 16) ^ 0x9E3779B9u;
    int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (u >= 0) { unsigned v; if (read(u, &v, sizeof v) == (ssize_t)sizeof v) w->rng ^= v; close(u); }
    if (!w->rng) w->rng = 1;
}
static unsigned rnd(ws_t *w) { unsigned x = w->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; w->rng = x; return x; }

int ws_connect(ws_t *w, const char *url, int timeout_ms) {
    char host[128]; int port; char *path;
    memset(w, 0, sizeof *w); w->fd = -1;
    if (ws_parse_url(url, host, sizeof host, &port, &path)) return -1;
    long long dl = now_ms() + timeout_ms;
    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) { free(path); return -1; }
    ws_init_fd(w, fd);
    unsigned char kr[16]; for (int i = 0; i < 16; i++) kr[i] = (unsigned char)rnd(w);
    char key[32]; b64_encode(kr, 16, key);
    sbuf rq = {0};
    int v6 = strchr(host, ':') != NULL;
    sb_printf(&rq, "GET %s HTTP/1.1\r\nHost: %s%s%s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
              path, v6 ? "[" : "", host, v6 ? "]" : "", port, key);
    free(path);
    int r = write_all(fd, rq.p, rq.n, dl);
    sb_free(&rq);
    if (r) { ws_close(w); return -1; }
    /* read response headers into rb */
    char *end = NULL;
    while (!end) {
        if (w->rlen >= w->rcap - 1) { ws_close(w); return slim_seterr("websocket handshake: headers too large"); }
        ssize_t n = read_some(fd, w->rb + w->rlen, w->rcap - 1 - w->rlen, dl);
        if (n <= 0) { if (n == 0) slim_seterr("websocket handshake: connection closed"); ws_close(w); return -1; }
        w->rlen += (size_t)n; w->rb[w->rlen] = 0;
        end = strstr((char *)w->rb, "\r\n\r\n");
    }
    char *hs = (char *)w->rb;
    if (strncmp(hs, "HTTP/1.1 101", 12)) {
        char *nl = strstr(hs, "\r\n"); if (nl) *nl = 0;
        slim_seterr("websocket handshake refused: %s", hs);
        ws_close(w); return -1;
    }
    /* verify accept */
    char acc_in[128]; snprintf(acc_in, sizeof acc_in, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char dig[20]; slim_sha1((unsigned char *)acc_in, strlen(acc_in), dig);
    char want[32]; b64_encode(dig, 20, want);
    int ok = 0;
    for (char *l = strstr(hs, "\r\n"); l && l < end; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, "sec-websocket-accept:", 21)) {
            char *v = l + 23; while (*v == ' ') v++;
            ok = !strncmp(v, want, strlen(want));
        }
    }
    if (!ok) { ws_close(w); return slim_seterr("websocket handshake: bad Sec-WebSocket-Accept"); }
    w->rpos = (size_t)(end + 4 - hs);
    return 0;
}

size_t ws_frame_header(unsigned char *h, int opcode, int fin, size_t len, const unsigned char *mask) {
    size_t k = 0;
    h[k++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 15));
    unsigned char m = mask ? 0x80 : 0;
    if (len < 126) h[k++] = (unsigned char)(m | len);
    else if (len <= 0xFFFF) { h[k++] = m | 126; h[k++] = (unsigned char)(len >> 8); h[k++] = (unsigned char)len; }
    else { h[k++] = m | 127; for (int i = 7; i >= 0; i--) h[k++] = (unsigned char)((uint64_t)len >> (8 * i)); }
    if (mask) { memcpy(h + k, mask, 4); k += 4; }
    return k;
}

int ws_send(ws_t *w, int opcode, const void *data, size_t len, int timeout_ms) {
    if (w->fd < 0) return slim_seterr("websocket not connected");
    unsigned char mask[4]; unsigned r = rnd(w); memcpy(mask, &r, 4);
    unsigned char hdr[14];
    size_t hl = ws_frame_header(hdr, opcode, 1, len, mask);
    unsigned char *buf = xmalloc(hl + len);
    memcpy(buf, hdr, hl);
    const unsigned char *s = data;
    for (size_t i = 0; i < len; i++) buf[hl + i] = s[i] ^ mask[i & 3];
    int rc = write_all(w->fd, buf, hl + len, now_ms() + timeout_ms);
    free(buf);
    return rc;
}

/* read exactly n bytes from buffer/socket */
static int rd(ws_t *w, void *dst, size_t n, long long dl) {
    unsigned char *d = dst;
    size_t av = w->rlen - w->rpos;
    if (av) { size_t k = av < n ? av : n; memcpy(d, w->rb + w->rpos, k); w->rpos += k; d += k; n -= k; }
    while (n) {
        if (n >= w->rcap) { /* large payload: read straight into destination */
            ssize_t r = read_some(w->fd, d, n, dl);
            if (r <= 0) { if (r == 0) slim_seterr("websocket closed by peer"); return -1; }
            d += r; n -= (size_t)r; continue;
        }
        ssize_t r = read_some(w->fd, w->rb, w->rcap, dl);
        if (r <= 0) { if (r == 0) slim_seterr("websocket closed by peer"); return -1; }
        w->rpos = 0; w->rlen = (size_t)r;
        size_t k = (size_t)r < n ? (size_t)r : n;
        memcpy(d, w->rb, k); w->rpos = k; d += k; n -= k;
    }
    if (w->rpos == w->rlen) w->rpos = w->rlen = 0;
    return 0;
}

int ws_recv(ws_t *w, int timeout_ms, char **msg, size_t *len) {
    if (w->fd < 0) return slim_seterr("websocket not connected");
    long long dl = now_ms() + timeout_ms;
    sbuf m = {0};
    int started = 0;
    for (;;) {
        if (!started && w->rpos == w->rlen) {
            int r = wait_fd(w->fd, POLLIN, dl);
            if (r == 0) { sb_free(&m); return 0; }
            if (r < 0) { sb_free(&m); return slim_seterr("poll: %s", strerror(errno)); }
        }
        /* once a frame begins, allow generous time for the rest of it */
        long long fdl = now_ms() + (timeout_ms > 60000 ? timeout_ms : 60000);
        unsigned char h[2];
        if (rd(w, h, 2, fdl)) goto fail;
        int fin = h[0] & 0x80, op = h[0] & 15, masked = h[1] & 0x80;
        if (h[0] & 0x70) { slim_seterr("websocket: reserved bits set"); goto fail; }
        uint64_t pl = h[1] & 0x7F;
        if (pl == 126) { unsigned char e[2]; if (rd(w, e, 2, fdl)) goto fail; pl = (uint64_t)e[0] << 8 | e[1]; }
        else if (pl == 127) {
            unsigned char e[8]; if (rd(w, e, 8, fdl)) goto fail;
            pl = 0; for (int i = 0; i < 8; i++) pl = pl << 8 | e[i];
            if (pl >> 63) { slim_seterr("websocket: bad length"); goto fail; }
        }
        unsigned char mk[4] = {0};
        if (masked && rd(w, mk, 4, fdl)) goto fail;
        if (op >= 8) {
            unsigned char cb[125];
            if (pl > 125 || !fin) { slim_seterr("websocket: bad control frame"); goto fail; }
            if (rd(w, cb, (size_t)pl, fdl)) goto fail;
            if (masked) for (size_t i = 0; i < pl; i++) cb[i] ^= mk[i & 3];
            if (op == WS_PING) { if (ws_send(w, WS_PONG, cb, (size_t)pl, 5000)) goto fail; }
            else if (op == WS_CLOSE) {
                ws_send(w, WS_CLOSE, cb, pl >= 2 ? 2 : 0, 1000);
                slim_seterr("websocket closed by peer");
                goto fail;
            } else if (op != WS_PONG) { slim_seterr("websocket: unknown control opcode %d", op); goto fail; }
            continue;
        }
        if (op == 0 && !started) { slim_seterr("websocket: unexpected continuation"); goto fail; }
        if (op != 0 && started) { slim_seterr("websocket: interleaved message"); goto fail; }
        if (op != 0 && op != WS_TEXT && op != WS_BIN) { slim_seterr("websocket: bad opcode %d", op); goto fail; }
        if (pl > w->max_msg || m.n + pl > w->max_msg) { slim_seterr("websocket: message too large"); goto fail; }
        started = 1;
        sb_reserve(&m, (size_t)pl);
        if (rd(w, m.p + m.n, (size_t)pl, fdl)) goto fail;
        if (masked) for (size_t i = 0; i < pl; i++) m.p[m.n + i] ^= (char)mk[i & 3];
        m.n += (size_t)pl; m.p[m.n] = 0;
        if (fin) { *msg = m.p; if (len) *len = m.n; return 1; }
    }
fail:
    sb_free(&m);
    close(w->fd); w->fd = -1;
    return -1;
}

void ws_close(ws_t *w) {
    if (w->fd >= 0) {
        unsigned char code[2] = {0x03, 0xE8};
        ws_send(w, WS_CLOSE, code, 2, 200);
        close(w->fd);
    }
    w->fd = -1;
    free(w->rb); w->rb = NULL;
    w->rpos = w->rlen = w->rcap = 0;
}
