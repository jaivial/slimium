#include "b64.h"

static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const unsigned char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = tbl[v >> 18]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63]; out[o++] = tbl[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0);
        out[o++] = tbl[v >> 18]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int dec(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

long b64_decode(const char *in, size_t n, unsigned char *out) {
    unsigned acc = 0; int bits = 0, pad = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; continue; }
        if (pad) return -1;
        int d = dec(c);
        if (d < 0) return -1;
        acc = (acc << 6) | (unsigned)d; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (bits >= 6 || pad > 2) return -1;
    return (long)o;
}
