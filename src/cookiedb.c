/* Minimal read-only SQLite reader for Chromium's Cookies database (see cookiedb.h).
 * File format: https://www.sqlite.org/fileformat2.html. Every offset is bounds-checked and the
 * number of pages visited is capped by the file's page count, so a corrupt file fails instead
 * of looping or reading out of bounds. */
#include "cookiedb.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAYLOAD_MAX (1u << 20)

typedef struct {
    int fd;
    uint32_t pgsz, usable, npages, visits;
    uint32_t root;          /* cookies table root page (0: none) */
    int col;                /* encrypted_value column; -1: any blob column */
    cookie_scan_t *s;
} db_t;

static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; while (n--) v = v << 8 | *p++; return v; }

static int varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 9 && p + i < end; i++) {
        if (i == 8) { *v = x << 8 | p[i]; return 9; }
        x = x << 7 | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) { *v = x; return i + 1; }
    }
    return 0;
}

static size_t stype_len(uint64_t t) {
    static const uint8_t l[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return t < 12 ? l[t] : (size_t)((t - 12) / 2);
}

/* Column i of a record: 0 found, 1 absent, -1 malformed. */
static int column(const uint8_t *r, size_t n, int i, uint64_t *t, const uint8_t **v, size_t *vn) {
    uint64_t hs; int k = varint(r, r + n, &hs);
    if (!k || hs > n || hs < (uint64_t)k) return -1;
    const uint8_t *h = r + k, *he = r + hs;
    uint64_t off = hs;
    for (int c = 0; h < he; c++) {
        uint64_t st; int m = varint(h, he, &st);
        if (!m) return -1;
        h += m;
        uint64_t l = stype_len(st);
        if (off + l > n) return -1;
        if (c == i) { *t = st; *v = r + off; *vn = (size_t)l; return 0; }
        off += l;
    }
    return 1;
}

static int read_page(db_t *d, uint32_t no, uint8_t *buf) {
    if (!no || no > d->npages || ++d->visits > 2 * d->npages + 8) return slim_seterr("corrupt cookie database (page %u)", no);
    if (pread(d->fd, buf, d->pgsz, (off_t)(no - 1) * d->pgsz) != (ssize_t)d->pgsz)
        return slim_seterr("read cookie database: %s", errno ? strerror(errno) : "short read");
    return 0;
}

static void count_blob(db_t *d, uint64_t t, const uint8_t *v, size_t n) {
    if (t < 12 || (t & 1) || !n) return;              /* not a non-empty blob */
    if (n >= 3 && !memcmp(v, "v10", 3)) d->s->v10++;
    else if (n >= 3 && !memcmp(v, "v11", 3)) d->s->v11++;
    else d->s->other++;
}

static int on_record(db_t *d, const uint8_t *r, size_t n, int master) {
    uint64_t t; const uint8_t *v; size_t vn;
    if (master) {   /* sqlite_master(type, name, tbl_name, rootpage, sql) */
        uint64_t tt, nt, rt, st; const uint8_t *tv, *nv, *rv, *sv; size_t tn, nn, rn, sn;
        if (column(r, n, 0, &tt, &tv, &tn) || column(r, n, 1, &nt, &nv, &nn) ||
            column(r, n, 3, &rt, &rv, &rn) || column(r, n, 4, &st, &sv, &sn)) return 0;
        if (tn != 5 || memcmp(tv, "table", 5) || nn != 7 || memcmp(nv, "cookies", 7)) return 0;
        if (rt < 1 || rt > 6) return slim_seterr("corrupt cookie database schema");
        d->root = be(rv, (int)(rn > 4 ? 4 : rn));
        const uint8_t *e = sn >= 15 ? memmem(sv, sn, "encrypted_value", 15) : NULL;
        const uint8_t *o = sn ? memchr(sv, '(', sn) : NULL;
        d->col = -1;
        if (e && o && o < e) { d->col = 0; for (; o < e; o++) if (*o == ',') d->col++; }
        return 0;
    }
    if (d->col >= 0) {
        int r2 = column(r, n, d->col, &t, &v, &vn);
        if (r2 < 0) return slim_seterr("corrupt cookie record");
        if (!r2) count_blob(d, t, v, vn);
        return 0;
    }
    for (int i = 0;; i++) {
        int r2 = column(r, n, i, &t, &v, &vn);
        if (r2 < 0) return slim_seterr("corrupt cookie record");
        if (r2) return 0;
        count_blob(d, t, v, vn);
    }
}

/* One table-leaf cell at buf[off]: assemble its payload (following overflow pages unless
 * local_only) and hand the record over. */
static int leaf_cell(db_t *d, const uint8_t *buf, uint32_t off, int master, int local_only) {
    const uint8_t *end = buf + d->usable, *p = buf + off;
    uint64_t len, rowid; int k = varint(p, end, &len);
    if (!k) return slim_seterr("corrupt cookie cell");
    p += k;
    if (!(k = varint(p, end, &rowid))) return slim_seterr("corrupt cookie cell");
    p += k;
    uint64_t u = d->usable, x = u - 35, local = len;
    if (len > x) {
        uint64_t m = (u - 12) * 32 / 255 - 23, kk = m + (len - m) % (u - 4);
        local = kk <= x ? kk : m;
    }
    if (local > (uint64_t)(end - p) || (local < len && 4 > (uint64_t)(end - p) - local)) return slim_seterr("corrupt cookie cell");
    if (local == len) return on_record(d, p, (size_t)len, master);
    if (local_only) return 0;
    if (len > PAYLOAD_MAX) return slim_seterr("cookie record too large");
    uint8_t *rec = malloc((size_t)len), *pg = malloc(d->pgsz);
    if (!rec || !pg) { free(rec); free(pg); return slim_seterr("out of memory"); }
    memcpy(rec, p, (size_t)local);
    size_t have = (size_t)local;
    uint32_t next = be(p + local, 4);
    int r = 0;
    while (!r && have < len) {
        if ((r = read_page(d, next, pg))) break;
        size_t chunk = d->usable - 4;
        if (chunk > len - have) chunk = (size_t)(len - have);
        memcpy(rec + have, pg + 4, chunk); have += chunk;
        next = be(pg, 4);
    }
    if (!r) r = on_record(d, rec, (size_t)len, master);
    free(rec); free(pg);
    return r;
}

/* Cells of one table b-tree page (header at h). */
static int page_cells(db_t *d, uint8_t *buf, uint32_t h, int master, int depth, int local_only);

static int walk(db_t *d, uint32_t no, int master, int depth) {
    if (depth > 32) return slim_seterr("corrupt cookie database (depth)");
    uint8_t *buf = malloc(d->pgsz);
    if (!buf) return slim_seterr("out of memory");
    int r = read_page(d, no, buf);
    if (!r) r = page_cells(d, buf, no == 1 ? 100 : 0, master, depth, 0);
    free(buf);
    return r;
}

static int page_cells(db_t *d, uint8_t *buf, uint32_t h, int master, int depth, int local_only) {
    uint8_t type = buf[h];
    if (type != 5 && type != 13) return local_only ? 0 : slim_seterr("corrupt cookie database (page type %u)", type);
    uint32_t n = be(buf + h + 3, 2), hdr = type == 5 ? 12 : 8;
    if (h + hdr + 2 * n > d->usable) return slim_seterr("corrupt cookie database (cells)");
    int r = 0;
    for (uint32_t i = 0; !r && i < n; i++) {
        uint32_t off = be(buf + h + hdr + 2 * i, 2);
        if (off < h + hdr || off + 4 > d->usable) return slim_seterr("corrupt cookie database (cell offset)");
        if (type == 13) r = leaf_cell(d, buf, off, master, local_only);
        else if (!local_only) r = walk(d, be(buf + off, 4), master, depth + 1);
    }
    if (!r && type == 5 && !local_only) r = walk(d, be(buf + h + 8, 4), master, depth + 1);
    return r;
}

/* After a crash a WAL-mode database keeps recent pages only in <path>-wal: scan the table-leaf
 * page images there too (records whose payload overflows are skipped; stale frames can only
 * add prefixes, which makes the guard more conservative, never less). */
static int scan_wal(db_t *d, const char *path) {
    char wal[4096];
    int n = snprintf(wal, sizeof wal, "%s-wal", path);
    if (n < 0 || (size_t)n >= sizeof wal) return 0;
    int fd = open(wal, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : slim_seterr("open %s: %s", wal, strerror(errno));
    uint8_t hdr[32];
    int r = 0;
    if (pread(fd, hdr, 32, 0) == 32 && (be(hdr, 4) & ~1u) == 0x377f0682u && be(hdr + 8, 4) == d->pgsz) {
        uint8_t *buf = malloc(d->pgsz);
        if (!buf) r = slim_seterr("out of memory");
        for (off_t o = 32; !r && pread(fd, buf, 24, o) == 24; o += 24 + d->pgsz) {
            uint32_t no = be(buf, 4);
            if (pread(fd, buf, d->pgsz, o + 24) != (ssize_t)d->pgsz) break;
            d->visits = 0;   /* best effort: a damaged frame is skipped, not fatal */
            page_cells(d, buf, no == 1 ? 100 : 0, 0, 0, 1);
        }
        free(buf);
    }
    close(fd);
    return r;
}

int cookiedb_scan(const char *path, cookie_scan_t *s) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : slim_seterr("open %s: %s", path, strerror(errno));
    struct stat st;
    uint8_t h[100];
    db_t d; memset(&d, 0, sizeof d);
    d.fd = fd; d.s = s; d.col = -1;
    int r = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) r = slim_seterr("cookie database is not a regular file");
    else if (st.st_size == 0) { close(fd); return 0; }
    else if (pread(fd, h, 100, 0) != 100 || memcmp(h, "SQLite format 3", 16)) r = slim_seterr("not an SQLite cookie database: %s", path);
    if (!r) {
        d.pgsz = be(h + 16, 2);
        if (d.pgsz == 1) d.pgsz = 65536;
        d.usable = d.pgsz - h[20];
        if (d.pgsz < 512 || (d.pgsz & (d.pgsz - 1)) || d.usable < 480) r = slim_seterr("corrupt cookie database header");
    }
    if (!r) {
        d.npages = (uint32_t)(st.st_size / d.pgsz);
        r = walk(&d, 1, 1, 0);
        d.visits = 0;
        if (!r && d.root) r = walk(&d, d.root, 0, 0);
        if (!r && h[18] == 2) r = scan_wal(&d, path);  /* WAL mode */
    }
    close(fd);
    return r;
}
