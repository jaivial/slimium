/* slim-httpd: tiny static file server for the offline workload (C11 + POSIX).
 *   slim-httpd DIR PORT ADDR...      e.g. slim-httpd www 9340 127.0.0.1 127.0.0.2
 * Serves DIR on PORT at every ADDR (each loopback address is a separate "site"
 * for Chromium's process model). Files under /dl/ are sent as attachments.
 * One request per connection, handled in order; GET/HEAD only. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *ctype(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js")) return "text/javascript";
    if (!strcmp(e, ".css")) return "text/css";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".svg")) return "image/svg+xml";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".txt")) return "text/plain";
    return "application/octet-stream";
}

static void reply(int c, const char *status, const char *type, long len, const char *extra) {
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%s"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len, extra);
    if (write(c, h, (size_t)n) < 0) return;
}

static void serve(int c, const char *root) {
    struct timeval tv = {5, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[4096]; size_t k = 0;
    while (k + 1 < sizeof req) {
        ssize_t r = read(c, req + k, sizeof req - 1 - k);
        if (r <= 0) return;
        k += (size_t)r; req[k] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    int head = !strncmp(req, "HEAD ", 5);
    if (strncmp(req, "GET ", 4) && !head) { reply(c, "405 Method Not Allowed", "text/plain", 0, ""); return; }
    char *u = req + (head ? 5 : 4), *e = strpbrk(u, " ?#");
    if (!e || *u != '/' || strstr(u, "..")) { reply(c, "400 Bad Request", "text/plain", 0, ""); return; }
    *e = 0;
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, u, u[strlen(u) - 1] == '/' ? "index.html" : "");
    int f = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) {
        if (f >= 0) close(f);
        reply(c, "404 Not Found", "text/plain", 9, "");
        if (!head && write(c, "not found", 9) < 0) return;
        return;
    }
    reply(c, "200 OK", ctype(path), (long)st.st_size,
          !strncmp(u, "/dl/", 4) ? "Content-Disposition: attachment\r\n" : "");
    char buf[16384]; ssize_t r;
    while (!head && (r = read(f, buf, sizeof buf)) > 0)
        if (write(c, buf, (size_t)r) != r) break;
    close(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: slim-httpd DIR PORT ADDR...\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int n = argc - 3, port = atoi(argv[2]);
    struct pollfd pf[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
        if (inet_pton(AF_INET, argv[3 + i], &a.sin_addr) != 1 || bind(s, (struct sockaddr *)&a, sizeof a) ||
            listen(s, 64)) {
            fprintf(stderr, "slim-httpd: cannot listen on %s:%d: %s\n", argv[3 + i], port, strerror(errno));
            return 1;
        }
        pf[i].fd = s; pf[i].events = POLLIN;
    }
    for (;;) {
        if (poll(pf, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; return 1; }
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            int c = accept(pf[i].fd, NULL, NULL);
            if (c < 0) continue;
            serve(c, argv[1]);
            close(c);
        }
    }
}
