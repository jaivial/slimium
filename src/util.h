/* Small helpers: growable string buffer, errors, time, files. */
#ifndef SLIM_UTIL_H
#define SLIM_UTIL_H
#include <stddef.h>

typedef struct { char *p; size_t n, cap; } sbuf;

void sb_add(sbuf *b, const void *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_putc(sbuf *b, char c);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_jstr(sbuf *b, const char *s, size_t n); /* append as quoted JSON string */
void sb_jstrz(sbuf *b, const char *s);          /* NULL -> null */
void sb_reserve(sbuf *b, size_t extra);
void sb_reset(sbuf *b);
void sb_free(sbuf *b);
const char *sb_cstr(sbuf *b);                   /* never NULL */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Last error message (one line). slim_seterr always returns -1. */
int slim_seterr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *slim_errmsg(void);

long long now_ms(void);
void sleep_ms(int ms);

int read_file(const char *path, char **data, size_t *len, size_t max);
int read_fd_all(int fd, char **data, size_t *len, size_t max);
int write_file(const char *path, const void *data, size_t len, int atomic);
int mkdir_p(const char *path, int mode);
const char *path_base(const char *p);
char *find_in_path(const char *name);       /* malloc'd absolute path or NULL */
int parse_int(const char *s, long *out);     /* strict, 0 ok */
int parse_double(const char *s, double *out);

#endif
