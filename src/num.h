/* Exact 128-bit signed integers: parse, print, checked arithmetic (E1). */
#ifndef TUPU_NUM_H
#define TUPU_NUM_H
#include "common.h"

#define NUM_MAX ((I128)(((U128)1 << 127) - 1))
#define NUM_MIN (-NUM_MAX - 1)
#define NUM_BUF 48

/* optional '-' then one or more decimal digits, whole of s[0..n); false when malformed or out of range */
bool num_parse(const char *s, size_t n, I128 *out);
size_t num_format(I128 v, char *buf); /* writes the digits, returns the length; no terminator */
bool num_add(I128 a, I128 b, I128 *out);
bool num_sub(I128 a, I128 b, I128 *out);
bool num_mul(I128 a, I128 b, I128 *out);
bool num_div(I128 a, I128 b, I128 *out); /* truncates toward zero; false on zero divisor or overflow */
bool num_rem(I128 a, I128 b, I128 *out); /* sign of the dividend; false on zero divisor */
bool num_neg(I128 a, I128 *out);

#endif
