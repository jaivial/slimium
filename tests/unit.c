/* Unit tests: JSON reader/builder, base64, SHA-1, WebSocket framing (over a socketpair). */
#include "b64.h"
#include "json.h"
#include "util.h"
#include "ws.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void t_json(void) {
    const char *doc = "{\"id\":7,\"result\":{\"result\":{\"type\":\"string\",\"value\":\"a\\\"b\\\\c\\n\\u00e9\\ud83d\\ude00\"}},"
                      "\"arr\":[1,-2.5e3,true,false,null,{\"k\":\"v\"},[]],\"e\":{},\"k\\u0065y\":1}";
    jv d, v;
    CHECK(json_parse(doc, strlen(doc), &d) == 0 && d.t == J_OBJ);
    CHECK(json_get(d, "id", &v) && v.t == J_NUM && json_int(v, 0) == 7);
    char *s = json_get_str(d, "result.result.value");
    CHECK(s && !strcmp(s, "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80"));
    free(s);
    CHECK(json_get(d, "arr.1", &v) && json_num(v, 0) == -2500);
    CHECK(json_get(d, "arr.2", &v) && json_bool(v));
    CHECK(json_get(d, "arr.3", &v) && v.t == J_BOOL && !json_bool(v));
    CHECK(json_get(d, "arr.4", &v) && v.t == J_NULL);
    CHECK(json_streq_path(d, "arr.5.k", "v"));
    CHECK(!json_get(d, "arr.7", &v));
    CHECK(!json_get(d, "nope", &v));
    CHECK(json_get(d, "key", &v) && json_int(v, 0) == 1); /* escaped key */
    CHECK(json_get(d, "arr", &v) && json_len(v) == 7);
    CHECK(json_get(d, "e", &v) && json_len(v) == 0);
    size_t pos = 0; jv k, val; int n = 0;
    while (json_next(d, &pos, &k, &val)) n++;
    CHECK(n == 5);
    /* invalid documents */
    const char *bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"\\x\"", "01", "1.", "tru", "[1] x", "\"a\nb\"", "{\"a\":1,}", "\"\\u12G4\"", NULL};
    for (int i = 0; bad[i]; i++) CHECK(json_parse(bad[i], strlen(bad[i]), &d) != 0);
    const char *good[] = {"0", "-0.5E+2", "\"\"", "[]", " {\"a\":[{}]} ", "\"\\/\"", NULL};
    for (int i = 0; good[i]; i++) CHECK(json_parse(good[i], strlen(good[i]), &d) == 0);
    /* depth limit: no stack overflow on hostile input */
    size_t deep = 100000;
    char *dp = xmalloc(deep * 2 + 1);
    memset(dp, '[', deep); memset(dp + deep, ']', deep); dp[2 * deep] = 0;
    CHECK(json_parse(dp, 2 * deep, &d) != 0);
    free(dp);
    /* builder escaping round-trips */
    const char raw[] = "q\"b\\s/\x01\t\n\r\x7f\xc3\xa9";
    sbuf b = {0};
    sb_jstr(&b, raw, sizeof raw - 1);
    CHECK(json_parse(b.p, b.n, &d) == 0 && d.t == J_STR);
    s = json_str(d);
    CHECK(s && !memcmp(s, raw, sizeof raw));
    free(s);
    sb_free(&b);
    /* lone surrogate becomes U+FFFD */
    char out[16];
    long l = json_unescape("\\ud800x", 7, out);
    CHECK(l == 4 && !memcmp(out, "\xef\xbf\xbdx", 4));
}

static void t_b64(void) {
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *ex[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    char buf[64]; unsigned char dec[64];
    for (int i = 0; i < 7; i++) {
        size_t n = b64_encode((const unsigned char *)in[i], strlen(in[i]), buf);
        CHECK(n == strlen(ex[i]) && !strcmp(buf, ex[i]));
        long d = b64_decode(ex[i], strlen(ex[i]), dec);
        CHECK(d == (long)strlen(in[i]) && !memcmp(dec, in[i], (size_t)d));
    }
    CHECK(b64_decode("Zm9v\nYmFy", 9, dec) == 6);
    CHECK(b64_decode("Zm9v!", 5, dec) < 0);
    CHECK(b64_decode("Zg=a", 4, dec) < 0);
    /* in-place on random data */
    unsigned char rnd[3001], *enc = xmalloc(5000);
    for (int i = 0; i < 3001; i++) rnd[i] = (unsigned char)(i * 131 + 7);
    size_t n = b64_encode(rnd, sizeof rnd, (char *)enc);
    long d = b64_decode((char *)enc, n, enc);
    CHECK(d == 3001 && !memcmp(enc, rnd, 3001));
    free(enc);
}

static void hex(const unsigned char *d, char *o) { for (int i = 0; i < 20; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static void t_sha1(void) {
    unsigned char d[20]; char h[41];
    slim_sha1((const unsigned char *)"abc", 3, d); hex(d, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    slim_sha1((const unsigned char *)"", 0, d); hex(d, h);
    CHECK(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    slim_sha1((const unsigned char *)m, strlen(m), d); hex(d, h);
    CHECK(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    /* RFC 6455 handshake example */
    const char *k = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    slim_sha1((const unsigned char *)k, strlen(k), d);
    char acc[32]; b64_encode(d, 20, acc);
    CHECK(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

/* ---- websocket framing: child plays the server ---- */
static int wr(int fd, const void *p, size_t n) {
    const char *s = p;
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) return -1; s += w; n -= (size_t)w; }
    return 0;
}
static int rdx(int fd, void *p, size_t n) {
    char *s = p;
    while (n) { ssize_t r = read(fd, s, n); if (r <= 0) return -1; s += r; n -= (size_t)r; }
    return 0;
}
static int srv_frame(int fd, int op, int fin, const void *p, size_t n) {
    unsigned char h[14];
    size_t hl = ws_frame_header(h, op, fin, n, NULL);
    return wr(fd, h, hl) || wr(fd, p, n);
}
/* read one client frame, verify it is masked, return unmasked payload */
static unsigned char *srv_read(int fd, int *op, size_t *len) {
    unsigned char h[2], e[8], mk[4];
    if (rdx(fd, h, 2)) return NULL;
    *op = h[0] & 15;
    if (!(h[1] & 0x80)) return NULL;
    uint64_t n = h[1] & 0x7f;
    if (n == 126) { if (rdx(fd, e, 2)) return NULL; n = (uint64_t)e[0] << 8 | e[1]; }
    else if (n == 127) { if (rdx(fd, e, 8)) return NULL; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | e[i]; }
    if (rdx(fd, mk, 4)) return NULL;
    unsigned char *p = malloc(n + 1);
    if (!p || rdx(fd, p, n)) { free(p); return NULL; }
    for (uint64_t i = 0; i < n; i++) p[i] ^= mk[i & 3];
    *len = n;
    return p;
}

static int server(int fd, size_t big) {
    int op; size_t n;
    if (srv_frame(fd, WS_TEXT, 1, "hello", 5)) return 1;
    /* fragmented message with a ping in the middle */
    if (srv_frame(fd, WS_TEXT, 0, "Hel", 3) || srv_frame(fd, WS_PING, 1, "pg", 2) || srv_frame(fd, 0, 0, "lo ", 3)) return 2;
    unsigned char *pong = srv_read(fd, &op, &n);
    if (!pong || op != WS_PONG || n != 2 || memcmp(pong, "pg", 2)) return 3;
    free(pong);
    if (srv_frame(fd, 0, 1, "world", 5)) return 4;
    /* 16-bit and 64-bit lengths */
    char *m = malloc(big);
    for (size_t i = 0; i < big; i++) m[i] = (char)('a' + i % 26);
    if (srv_frame(fd, WS_TEXT, 1, m, 300) || srv_frame(fd, WS_BIN, 1, m, big)) return 5;
    free(m);
    /* client -> server, 64-bit length, masked */
    unsigned char *c = srv_read(fd, &op, &n);
    if (!c || op != WS_TEXT || n != 70000) return 6;
    for (size_t i = 0; i < n; i++) if (c[i] != (unsigned char)('A' + i % 7)) return 7;
    free(c);
    if (srv_frame(fd, WS_CLOSE, 1, "\x03\xe8", 2)) return 8;
    unsigned char *cl = srv_read(fd, &op, &n);
    if (!cl || op != WS_CLOSE) return 9;
    free(cl);
    return 0;
}

static void t_ws(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    size_t big = (5u << 20) + 7; /* > 65535: 64-bit length */
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); _exit(server(sv[1], big)); }
    close(sv[1]);
    ws_t w; ws_init_fd(&w, sv[0]);
    char *msg; size_t len;
    CHECK(ws_recv(&w, 5000, &msg, &len) == 1 && len == 5 && !strcmp(msg, "hello"));
    free(msg);
    CHECK(ws_recv(&w, 5000, &msg, &len) == 1 && len == 11 && !strcmp(msg, "Hello world"));
    free(msg);
    CHECK(ws_recv(&w, 5000, &msg, &len) == 1 && len == 300 && msg[299] == (char)('a' + 299 % 26));
    free(msg);
    int r = ws_recv(&w, 5000, &msg, &len);
    CHECK(r == 1 && len == big);
    if (r == 1) {
        int ok = 1;
        for (size_t i = 0; i < len; i++) if (msg[i] != (char)('a' + i % 26)) { ok = 0; break; }
        CHECK(ok && msg[len] == 0);
        free(msg);
    }
    char *out = xmalloc(70000);
    for (size_t i = 0; i < 70000; i++) out[i] = (char)('A' + i % 7);
    CHECK(ws_send(&w, WS_TEXT, out, 70000, 5000) == 0);
    free(out);
    CHECK(ws_recv(&w, 5000, &msg, &len) == -1); /* close frame */
    int st = -1;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) fprintf(stderr, "server step failed: %d\n", WEXITSTATUS(st));
    ws_close(&w);

    /* timeout with nothing to read */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ws_init_fd(&w, sv[0]);
    long long t0 = now_ms();
    CHECK(ws_recv(&w, 100, &msg, &len) == 0);
    CHECK(now_ms() - t0 >= 90);
    /* peer closes mid-frame */
    unsigned char h[4] = {0x81, 126, 0x10, 0x00};
    CHECK(write(sv[1], h, 4) == 4 && write(sv[1], "abc", 3) == 3);
    close(sv[1]);
    CHECK(ws_recv(&w, 1000, &msg, &len) == -1);
    ws_close(&w);

    /* URL parsing */
    char host[64]; int port; char *path;
    CHECK(ws_parse_url("ws://127.0.0.1:9222/devtools/browser/x", host, sizeof host, &port, &path) == 0 &&
          !strcmp(host, "127.0.0.1") && port == 9222 && !strcmp(path, "/devtools/browser/x"));
    free(path);
    CHECK(ws_parse_url("ws://[::1]:9300", host, sizeof host, &port, &path) == 0 && !strcmp(host, "::1") && port == 9300 && !strcmp(path, "/"));
    free(path);
    CHECK(ws_parse_url("wss://x", host, sizeof host, &port, &path) != 0);
    CHECK(tcp_connect("example.com", 80, 100) < 0); /* names other than localhost are refused */
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    t_json(); t_b64(); t_sha1(); t_ws();
    printf("unit: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
