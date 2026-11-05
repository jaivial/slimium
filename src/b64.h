#ifndef SLIM_B64_H
#define SLIM_B64_H
#include <stddef.h>
/* Encode n bytes into out (needs 4*((n+2)/3)+1 bytes). Returns output length. */
size_t b64_encode(const unsigned char *in, size_t n, char *out);
/* Decode (whitespace ignored, padding optional). out may alias in. Returns length or -1. */
long b64_decode(const char *in, size_t n, unsigned char *out);
#endif
