/* Unit tests of the library modules: the splitmix64 known answers of emitter.md 4, exact integers. */
#include "../src/common.h"
#include "../src/num.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_mix(void) {
  CHECK(mix64(0) == 0xe220a8397b1dcdafULL);
  CHECK(mix64(1) == 0x910a2dec89025cc1ULL);
  CHECK(mix64(1ULL << 63) == 0x481ec0a212a9f3dbULL);
  CHECK(mix64(~0ULL) == 0xe4d971771b652c20ULL);
}

static void test_num(void) {
  I128 v;
  char b[NUM_BUF];
  CHECK(num_parse("-0", 2, &v) && v == 0);
  CHECK(num_parse("0007", 4, &v) && v == 7);
  CHECK(!num_parse("", 0, &v) && !num_parse("-", 1, &v) && !num_parse("+1", 2, &v) && !num_parse("1 ", 2, &v));
  CHECK(num_parse("170141183460469231731687303715884105727", 39, &v) && v == NUM_MAX);
  CHECK(!num_parse("170141183460469231731687303715884105728", 39, &v));
  CHECK(num_parse("-170141183460469231731687303715884105728", 40, &v) && v == NUM_MIN);
  CHECK(!num_parse("-170141183460469231731687303715884105729", 40, &v));
  size_t n = num_format(NUM_MIN, b);
  CHECK(n == 40 && memcmp(b, "-170141183460469231731687303715884105728", 40) == 0);
  n = num_format(0, b);
  CHECK(n == 1 && b[0] == '0');
  CHECK(!num_add(NUM_MAX, 1, &v) && num_add(NUM_MAX, -1, &v));
  CHECK(!num_sub(NUM_MIN, 1, &v));
  CHECK(!num_mul(NUM_MAX, 2, &v) && num_mul(-3, 4, &v) && v == -12);
  CHECK(!num_div(1, 0, &v) && !num_div(NUM_MIN, -1, &v) && num_div(-7, 2, &v) && v == -3);
  CHECK(!num_rem(1, 0, &v) && num_rem(-7, 2, &v) && v == -1 && num_rem(NUM_MIN, -1, &v) && v == 0);
  CHECK(!num_neg(NUM_MIN, &v) && num_neg(5, &v) && v == -5);
}

int main(void) {
  test_mix();
  test_num();
  printf("unit failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
