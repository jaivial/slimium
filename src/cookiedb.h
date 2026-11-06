/* Read-only, dependency-free inspection of Chromium's Cookies SQLite database: how many
 * encrypted_value blobs carry each OSCrypt prefix (v10 = fixed "basic" key, v11 = OS keyring).
 * Parses just enough of the SQLite file format (b-tree pages, records, overflow chains, WAL
 * frames); never returns, prints, or decrypts cookie values. */
#ifndef SLIM_COOKIEDB_H
#define SLIM_COOKIEDB_H

typedef struct { unsigned long v10, v11, other; } cookie_scan_t;

/* Add the prefix counts of the database at path to *s (a missing or empty file adds nothing).
 * 0 ok, -1 unreadable/corrupt (slim_seterr). */
int cookiedb_scan(const char *path, cookie_scan_t *s);

#endif
