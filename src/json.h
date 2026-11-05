/* Compact zero-allocation JSON reader: values are views into the source text. */
#ifndef SLIM_JSON_H
#define SLIM_JSON_H
#include <stddef.h>
#include "util.h"

typedef enum { J_NONE = 0, J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct { const char *p; size_t n; jtype t; } jv; /* p/n: raw text of the value */

/* Validate a whole document (trailing whitespace only). 0 ok, -1 error. */
int json_parse(const char *s, size_t n, jv *out);
/* Lookup by dotted path ("result.result.value", "targetInfos.0.url"). 1 found, 0 not. */
int json_get(jv v, const char *path, jv *out);
/* Iterate an object/array. *pos starts at 0. key is J_NONE for arrays. 1 item, 0 end. */
int json_next(jv c, size_t *pos, jv *key, jv *val);
size_t json_len(jv c); /* number of items in array/object */

/* Unescape a JSON string body (without quotes) into out (may alias in; out size >= n). Returns length or -1. */
long json_unescape(const char *in, size_t n, char *out);
char *json_str(jv v);                       /* malloc'd: unescaped string, raw text otherwise, NULL if J_NONE */
int json_str_eq(jv v, const char *s);        /* v is a string equal to s */
int json_streq_path(jv v, const char *path, const char *s);
char *json_get_str(jv v, const char *path);  /* malloc'd string at path or NULL */
double json_num(jv v, double def);
long long json_int(jv v, long long def);
int json_bool(jv v);                          /* true -> 1, else 0 */
double json_get_num(jv v, const char *path, double def);
/* Append an unescaped string value to b (raw text if not a string). */
void sb_jv_text(sbuf *b, jv v);

#endif
