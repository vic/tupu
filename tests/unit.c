/* Unit tests of the library modules: the splitmix64 known answers of emitter.md 4, exact integers. */
#include "../src/common.h"
#include "../src/num.h"
#include "../src/sym.h"
#include "../src/table.h"
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

static void test_sym_rank_and_empty_bitset(void) {
  Err err = {0};
  SymTab s;
  sym_init(&s);
  uint32_t id_b = 0, id_d = 0, id_f = 0;
  CHECK(sym_intern(&s, "b", 1, &id_b, &err) == STATUS_OK);
  CHECK(sym_intern(&s, "d", 1, &id_d, &err) == STATUS_OK);
  CHECK(sym_intern(&s, "f", 1, &id_f, &err) == STATUS_OK);
  sym_seal(&s);
  CHECK(sym_build_rank(&s, &err) == STATUS_OK);
  CHECK(s.rank[id_b] == 0 && s.rank[id_d] == 1 && s.rank[id_f] == 2);

  uint32_t id_a = 0, id_c = 0, id_e = 0;
  CHECK(sym_intern(&s, "a", 1, &id_a, &err) == STATUS_OK);
  CHECK(sym_intern(&s, "c", 1, &id_c, &err) == STATUS_OK);
  CHECK(sym_intern(&s, "e", 1, &id_e, &err) == STATUS_OK);
  CHECK(sym_build_rank(&s, &err) == STATUS_OK);
  CHECK(s.rank[id_a] == 0 && s.rank[id_b] == 1 && s.rank[id_c] == 2);
  CHECK(s.rank[id_d] == 3 && s.rank[id_e] == 4 && s.rank[id_f] == 5);

  sym_forget_evaluation(&s);
  CHECK(s.n == 3);
  CHECK(sym_build_rank(&s, &err) == STATUS_OK);
  CHECK(s.rank[id_b] == 0 && s.rank[id_d] == 1 && s.rank[id_f] == 2);
  sym_free(&s);

  Base kinds[2] = { BASE_SYMBOL, BASE_SYMBOL };
  Relation rel = {0};
  rel.ncols = 2;
  rel.colkind = kinds;
  rel.lat_col = -1;
  Table t;
  CHECK(table_init(&t, &rel, false, &err) == STATUS_OK);
  CHECK(table_enable_bitset(&t, 64, &err) == STATUS_OK);
  CHECK(t.bits == NULL);
  I128 row[2] = { 1, 2 };
  bool added = false;
  CHECK(table_add(&t, table_hash_row(&t, row), row, ORG_INPUT, 0, 0, NULL, 0, &added, &err) == STATUS_OK && added);
  CHECK(t.bits != NULL);
  CHECK(table_contains(&t, row));
  table_clear(&t);
  CHECK(t.count == 0);
  CHECK(!table_contains(&t, row));
  table_free(&t);
}

int main(void) {
  test_mix();
  test_num();
  test_sym_rank_and_empty_bitset();
  printf("unit failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
