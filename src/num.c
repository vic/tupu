#include "num.h"

bool num_parse(const char *s, size_t n, I128 *out) {
  size_t i = 0;
  bool neg = false;
  if (n > 0 && s[0] == '-') {
    neg = true;
    i = 1;
  }
  if (i >= n) return false;
  U128 mag = 0;
  U128 limit = neg ? ((U128)1 << 127) : (((U128)1 << 127) - 1);
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    unsigned d = (unsigned)(s[i] - '0');
    if (mag > (limit - d) / 10) return false;
    mag = mag * 10 + d;
  }
  *out = neg ? (I128)(0 - mag) : (I128)mag;
  return true;
}

size_t num_format(I128 v, char *buf) {
  char tmp[NUM_BUF];
  size_t n = 0;
  bool neg = v < 0;
  U128 m = neg ? (0 - (U128)v) : (U128)v;
  do {
    tmp[n++] = (char)('0' + (int)(m % 10));
    m /= 10;
  } while (m != 0);
  size_t len = 0;
  if (neg) buf[len++] = '-';
  while (n > 0) buf[len++] = tmp[--n];
  return len;
}

bool num_add(I128 a, I128 b, I128 *out) { return !__builtin_add_overflow(a, b, out); }
bool num_sub(I128 a, I128 b, I128 *out) { return !__builtin_sub_overflow(a, b, out); }
bool num_mul(I128 a, I128 b, I128 *out) { return !__builtin_mul_overflow(a, b, out); }
bool num_neg(I128 a, I128 *out) { return !__builtin_sub_overflow((I128)0, a, out); }

bool num_div(I128 a, I128 b, I128 *out) {
  if (b == 0) return false;
  if (a == NUM_MIN && b == -1) return false;
  *out = a / b;
  return true;
}

bool num_rem(I128 a, I128 b, I128 *out) {
  if (b == 0) return false;
  if (b == -1) {
    *out = 0;
    return true;
  }
  *out = a % b;
  return true;
}
