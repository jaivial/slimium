/* Minimal RFC 6455 WebSocket client + HTTP GET, loopback TCP only. */
#ifndef SLIM_WS_H
#define SLIM_WS_H
#include <stddef.h>

typedef struct {
    int fd;
    unsigned char *rb;     /* read buffer */
    size_t rpos, rlen, rcap;
    size_t max_msg;        /* max assembled message size */
    unsigned rng;
} ws_t;

enum { WS_TEXT = 1, WS_BIN = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

/* Connect TCP to a numeric host or "localhost". Returns fd or -1. */
int tcp_connect(const char *host, int port, int timeout_ms);
/* HTTP/1.1 GET; *body malloc'd NUL-terminated. Returns HTTP status or -1. */
int http_get(const char *host, int port, const char *path, int timeout_ms, char **body, size_t *len);

/* Parse ws://host:port/path. host buffer >= 64, path malloc'd. 0 ok. */
int ws_parse_url(const char *url, char *host, size_t hostn, int *port, char **path);
int ws_connect(ws_t *w, const char *url, int timeout_ms);
void ws_init_fd(ws_t *w, int fd);  /* wrap an already-connected stream (tests) */
int ws_send(ws_t *w, int opcode, const void *data, size_t len, int timeout_ms);
/* Receive one complete data message (handles fragments, ping->pong, close).
 * 1: *msg malloc'd NUL-terminated; 0: timeout before a frame started; -1: error/closed. */
int ws_recv(ws_t *w, int timeout_ms, char **msg, size_t *len);
void ws_close(ws_t *w);

/* exposed for tests */
void slim_sha1(const unsigned char *data, size_t n, unsigned char out[20]);
size_t ws_frame_header(unsigned char *h, int opcode, int fin, size_t len, const unsigned char *mask);

#endif
