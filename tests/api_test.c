/* Tests for the in-process C library API (include/tupu.h). */
#define _POSIX_C_SOURCE 200809L
#include "tupu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;

static void check(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    fails++;
  }
}

static long read_rss_kb(void) {
  FILE *f = fopen("/proc/self/status", "r");
  if (f == NULL) return -1;
  char line[256];
  long kb = -1;
  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      kb = strtol(line + 6, NULL, 10);
      break;
    }
  }
  (void)fclose(f);
  return kb;
}

typedef struct {
  int64_t a, b;
} Pair;

static int cmp_pair(const void *x, const void *y) {
  const Pair *p = x, *q = y;
  if (p->a != q->a) return p->a < q->a ? -1 : 1;
  if (p->b != q->b) return p->b < q->b ? -1 : 1;
  return 0;
}

static const char *TC_PROG =
    ".decl edge(x: symbol, y: symbol)\n"
    ".input edge\n"
    ".decl tc(x: symbol, y: symbol)\n"
    ".output tc\n"
    "tc(x, y) :- edge(x, y).\n"
    "tc(x, z) :- tc(x, y), edge(y, z).\n";

static void test_1000_frames(void) {
  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open(TC_PROG, strlen(TC_PROG), "tc.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "tupu_open tc");
  if (s == NULL) return;

  int edge_id = tupu_rel_id(s, "edge");
  int tc_id = tupu_rel_id(s, "tc");
  check(edge_id >= 0 && tc_id >= 0, "rel ids found");
  check(tupu_rel_id(s, "nonexistent") == -1, "unknown rel returns -1");
  check(tupu_rel_ncols(s, edge_id) == 2 && tupu_rel_ncols(s, tc_id) == 2, "rel ncols == 2");

  uint32_t sym[10];
  for (int i = 0; i < 10; i++) {
    char buf[16];
    int n = snprintf(buf, sizeof buf, "n%d", i);
    check(tupu_intern(s, buf, (size_t)n, &sym[i]) == 0, "tupu_intern");
    size_t slen = 0;
    const char *sp = tupu_sym_str(s, sym[i], &slen);
    check(sp != NULL && slen == (size_t)n && memcmp(sp, buf, slen) == 0, "tupu_sym_str roundtrip");
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  long rss_f10 = -1, rss_f1000 = -1;

  uint32_t frame_first_id = 0;
  for (int frame = 1; frame <= 1000; frame++) {
    check(tupu_reset(s) == 0, "tupu_reset");
    /* a frame interns its own symbols: reset forgot the ones of the frame before, so the ids start at the same place
       every frame (the session does not grow with its history) and an id of the frame before resolves to nothing */
    if (frame > 1) check(tupu_sym_str(s, sym[9], NULL) == NULL, "reset forgets the symbols of the frame before");
    for (int i = 0; i < 10; i++) {
      char buf[16];
      int n = snprintf(buf, sizeof buf, "f%d_n%d", frame, i);
      check(tupu_intern(s, buf, (size_t)n, &sym[i]) == 0, "tupu_intern in frame");
    }
    if (frame == 1) frame_first_id = sym[0];
    check(sym[0] == frame_first_id, "frame symbols take the same ids every frame");
    for (int i = 0; i < 9; i++) {
      int64_t row[2] = { (int64_t)sym[i], (int64_t)sym[i + 1] };
      check(tupu_add_row(s, edge_id, row, 2) == 0, "tupu_add_row");
    }
    int rc = tupu_eval(s, &ecode, ebuf, sizeof ebuf);
    check(rc == 0 && ecode == 0, "tupu_eval frame");
    uint32_t cnt = tupu_rel_count(s, tc_id);
    if (frame == 1 || frame == 1000) {
      check(cnt == 45, "tc count == 45 on frame 1 and 1000");
      Pair got[64];
      for (uint32_t r = 0; r < cnt && r < 64; r++) {
        int64_t v[2];
        check(tupu_rel_read_row(s, tc_id, r, v, 2) == 0, "tupu_rel_read_row");
        got[r].a = v[0];
        got[r].b = v[1];
      }
      qsort(got, cnt, sizeof got[0], cmp_pair);
      int idx = 0;
      for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j < 10; j++) {
          check(got[idx].a == (int64_t)sym[i] && got[idx].b == (int64_t)sym[j], "tc pair exact match");
          idx++;
        }
      }
    } else {
      check(cnt == 45, "tc count == 45");
    }
    if (frame == 10) rss_f10 = read_rss_kb();
    if (frame == 1000) rss_f1000 = read_rss_kb();
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
  printf("api_test 1000_frames wall=%.2fms rss_f10=%ldKB rss_f1000=%ldKB rss_delta=%ldKB\n",
         ms, rss_f10, rss_f1000, (rss_f10 >= 0 && rss_f1000 >= 0) ? (rss_f1000 - rss_f10) : 0L);
  tupu_close(s);
}

static void test_budget_exceeded(void) {
  char ebuf[256];
  int ecode = 0;
  TupuSession *s = tupu_open(TC_PROG, strlen(TC_PROG), "tc.dl", 10, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL, "tupu_open budget test");
  if (s == NULL) return;
  int edge_id = tupu_rel_id(s, "edge");
  uint32_t sym[10];
  for (int i = 0; i < 10; i++) {
    char buf[16];
    int n = snprintf(buf, sizeof buf, "b%d", i);
    check(tupu_intern(s, buf, (size_t)n, &sym[i]) == 0, "intern budget");
  }
  for (int i = 0; i < 9; i++) {
    int64_t row[2] = { (int64_t)sym[i], (int64_t)sym[i + 1] };
    check(tupu_add_row(s, edge_id, row, 2) == 0, "add budget");
  }
  int rc = tupu_eval(s, &ecode, ebuf, sizeof ebuf);
  check(rc == -1 && ecode == TUPU_CODE_DATALOG_BUDGET, "budget exceeded returns TUPU_CODE_DATALOG_BUDGET");
  tupu_close(s);
}

static void test_arith_overflow(void) {
  const char *prog =
      ".decl a(x: number)\n"
      ".input a\n"
      ".decl b(x: number)\n"
      ".output b\n"
      "b(x * x * x) :- a(x).\n";
  char ebuf[256];
  int ecode = 0;
  TupuSession *s = tupu_open(prog, strlen(prog), "arith.dl", 1000, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL, "tupu_open arith test");
  if (s == NULL) return;
  int a_id = tupu_rel_id(s, "a");
  int64_t row[1] = { 1000000000000000LL }; /* 10^15 -> (10^15)^3 = 10^45 > I128_MAX */
  check(tupu_add_row(s, a_id, row, 1) == 0, "add arith row");
  int rc = tupu_eval(s, &ecode, ebuf, sizeof ebuf);
  check(rc == -1 && ecode == TUPU_CODE_DATALOG_ARITH, "arith overflow returns TUPU_CODE_DATALOG_ARITH");
  tupu_close(s);
}

static Pair *run_tc_threads(size_t nthreads, uint32_t *out_n) {
  char ebuf[256];
  int ecode = 0;
  TupuSession *s = tupu_open(TC_PROG, strlen(TC_PROG), "tc.dl", 1000000, nthreads, ebuf, sizeof ebuf, &ecode);
  check(s != NULL, "tupu_open threads");
  if (s == NULL) return NULL;
  int edge_id = tupu_rel_id(s, "edge");
  int tc_id = tupu_rel_id(s, "tc");
  uint32_t sym[40];
  for (int i = 0; i < 40; i++) {
    char buf[16];
    int n = snprintf(buf, sizeof buf, "v%02d", i);
    check(tupu_intern(s, buf, (size_t)n, &sym[i]) == 0, "intern threads");
  }
  for (int i = 0; i < 40; i++) {
    int64_t r1[2] = { (int64_t)sym[i], (int64_t)sym[(i + 1) % 40] };
    int64_t r2[2] = { (int64_t)sym[i], (int64_t)sym[(i * 7 + 3) % 40] };
    check(tupu_add_row(s, edge_id, r1, 2) == 0, "add r1");
    check(tupu_add_row(s, edge_id, r2, 2) == 0, "add r2");
  }
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "eval threads");
  uint32_t cnt = tupu_rel_count(s, tc_id);
  *out_n = cnt;
  Pair *rows = malloc((size_t)cnt * sizeof *rows);
  check(rows != NULL, "alloc rows");
  for (uint32_t r = 0; r < cnt; r++) {
    int64_t v[2];
    check(tupu_rel_read_row(s, tc_id, r, v, 2) == 0, "read row threads");
    rows[r].a = v[0];
    rows[r].b = v[1];
  }
  qsort(rows, cnt, sizeof rows[0], cmp_pair);
  tupu_close(s);
  return rows;
}

static void test_threads_equal(void) {
  uint32_t n1 = 0, n4 = 0;
  Pair *r1 = run_tc_threads(1, &n1);
  Pair *r4 = run_tc_threads(4, &n4);
  check(n1 == 1600 && n1 == n4, "j1 and j4 row counts equal (1600)");
  if (r1 != NULL && r4 != NULL && n1 == n4) {
    int eq = memcmp(r1, r4, (size_t)n1 * sizeof *r1) == 0;
    check(eq, "j1 and j4 sorted rows identical");
  }
  free(r1);
  free(r4);
}

static void test_i128_and_replay(void) {
  const char *prog =
      ".decl val(x: number)\n"
      ".input val\n"
      ".decl big(x: number)\n"
      ".output big\n"
      "big(x + 1) :- val(x).\n";
  char ebuf[256];
  int ecode = 0;
  TupuSession *s = tupu_open_ex(prog, strlen(prog), "i128.dl", 1000, 1, 1,
                                ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "tupu_open_ex with prov=1");
  if (s == NULL) return;
  int val_id = tupu_rel_id(s, "val");
  int big_id = tupu_rel_id(s, "big");
  check(val_id >= 0 && big_id >= 0, "i128 rel ids found");

  TupuI128 r1[1] = { tupu_i128_from_u64(UINT64_MAX) };
  TupuI128 r2[1] = { tupu_i128_from_i64(INT64_MIN) };
  check(tupu_add_row_i128(s, val_id, r1, 1) == 0, "add UINT64_MAX i128");
  check(tupu_add_row_i128(s, val_id, r2, 1) == 0, "add INT64_MIN i128");
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "eval i128");
  check(tupu_rel_count(s, big_id) == 2, "big has 2 rows");

  int saw_2p64 = 0, saw_i64min_p1 = 0;
  for (uint32_t r = 0; r < tupu_rel_count(s, big_id); r++) {
    TupuI128 out[1];
    check(tupu_rel_read_row_i128(s, big_id, r, out, 1) == 0, "read i128 row");
    if (out[0].hi == 1 && out[0].lo == 0) saw_2p64 = 1;
    if (out[0].hi == -1 && out[0].lo == (uint64_t)(INT64_MIN + 1)) saw_i64min_p1 = 1;
  }
  check(saw_2p64 && saw_i64min_p1, "exact 128-bit UINT64_MAX+1 and INT64_MIN+1 verified");
  check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "O6 replay passes");
  check(tupu_last_err_code(s) == 0, "last_err_code == 0");
  tupu_close(s);
}

static void test_column_kinds(void) {
  const char *prog =
      ".decl mixed(a: symbol, n: number, b: symbol)\n"
      ".input mixed\n"
      ".decl tally(n: number)\n"
      ".output tally\n"
      "tally(n) :- mixed(_, n, _).\n";
  char ebuf[256];
  int ecode = 0;
  TupuSession *s = tupu_open(prog, strlen(prog), "colkind.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "colkind open");
  if (s == NULL) return;
  int mixed = tupu_rel_id(s, "mixed");
  int tally = tupu_rel_id(s, "tally");
  check(mixed >= 0 && tally >= 0, "colkind rel ids");
  check(tupu_rel_col_kind(s, mixed, 0) == TUPU_COL_SYMBOL, "colkind mixed symbol 0");
  check(tupu_rel_col_kind(s, mixed, 1) == TUPU_COL_NUMBER, "colkind mixed number 1");
  check(tupu_rel_col_kind(s, mixed, 2) == TUPU_COL_SYMBOL, "colkind mixed symbol 2");
  check(tupu_rel_col_kind(s, tally, 0) == TUPU_COL_NUMBER, "colkind tally number 0");
  check(tupu_rel_col_kind(s, mixed, 3) == -1 && tupu_rel_col_kind(s, mixed, -1) == -1, "colkind col out of range");
  check(tupu_rel_col_kind(s, -1, 0) == -1 && tupu_rel_col_kind(NULL, mixed, 0) == -1, "colkind bad rel");
  tupu_close(s);
}

static void test_i128_text(void) {
  char text[TUPU_I128_TEXT_MAX];
  TupuI128 v;
  size_t n;
  check(tupu_i128_parse("170141183460469231731687303715884105727", 39, &v) == 0 && v.hi == INT64_MAX && v.lo == UINT64_MAX, "i128 parse max");
  check(tupu_i128_parse("-170141183460469231731687303715884105728", 40, &v) == 0 && v.hi == INT64_MIN && v.lo == 0, "i128 parse min");
  check(tupu_i128_parse("18446744073709551615", 20, &v) == 0 && v.hi == 0 && v.lo == UINT64_MAX, "i128 parse u64 max");
  check(tupu_i128_parse("18446744073709551616", 20, &v) == 0 && v.hi == 1 && v.lo == 0, "i128 parse 2^64");
  check(tupu_i128_parse("170141183460469231731687303715884105728", 39, &v) == -1 && tupu_i128_parse("-170141183460469231731687303715884105729", 40, &v) == -1 && tupu_i128_parse("", 0, &v) == -1 && tupu_i128_parse("-", 1, &v) == -1 && tupu_i128_parse("1x", 2, &v) == -1 && tupu_i128_parse("+5", 2, &v) == -1 && tupu_i128_parse(NULL, 1, &v) == -1 && tupu_i128_parse("5", 1, NULL) == -1, "i128 parse rejects");
  check(tupu_i128_parse("123", 2, &v) == 0 && v.hi == 0 && v.lo == 12, "i128 parse length bounds");
  n = tupu_i128_format((TupuI128){UINT64_MAX, INT64_MAX}, text);
  check(n == 39 && memcmp(text, "170141183460469231731687303715884105727", 39) == 0, "i128 format max");
  n = tupu_i128_format((TupuI128){0, INT64_MIN}, text);
  check(n == 40 && memcmp(text, "-170141183460469231731687303715884105728", 40) == 0, "i128 format min");
  n = tupu_i128_format((TupuI128){0, 1}, text);
  check(n == 20 && memcmp(text, "18446744073709551616", 20) == 0, "i128 format 2^64");
  n = tupu_i128_format((TupuI128){0, 0}, text);
  int zero_ok = n == 1 && text[0] == '0';
  n = tupu_i128_format((TupuI128){UINT64_MAX, -1}, text);
  check(zero_ok && n == 2 && memcmp(text, "-1", 2) == 0, "i128 format zero and minus one");
}

static void test_sparse_multistratum_frames(void) {
  size_t cap = 32768;
  char *prog = malloc(cap);
  check(prog != NULL, "alloc sparse prog");
  if (prog == NULL) return;
  size_t pos = 0;
  for (int i = 0; i < 100; i++) {
    int n = snprintf(prog + pos, cap - pos,
                     ".decl in%d(x: symbol)\n"
                     ".input in%d\n"
                     ".decl out%d(x: symbol)\n"
                     ".output out%d\n"
                     "out%d(x) :- in%d(x).\n",
                     i, i, i, i, i, i);
    check(n > 0 && (size_t)n < cap - pos, "format sparse stratum");
    if (n <= 0 || (size_t)n >= cap - pos) {
      free(prog);
      return;
    }
    pos += (size_t)n;
  }
  int tail_n = snprintf(prog + pos, cap - pos,
                        ".decl bad(x: symbol)\n"
                        ".input bad\n"
                        ".decl ok(x: symbol)\n"
                        ".output ok\n"
                        "ok(x) :- out0(x), !bad(x).\n"
                        ".decl edge(x: symbol, y: symbol)\n"
                        ".input edge\n"
                        ".decl tc(x: symbol, y: symbol)\n"
                        ".output tc\n"
                        "tc(x, y) :- edge(x, y).\n"
                        "tc(x, z) :- tc(x, y), edge(y, z).\n");
  check(tail_n > 0 && (size_t)tail_n < cap - pos, "format sparse tail");
  if (tail_n <= 0 || (size_t)tail_n >= cap - pos) {
    free(prog);
    return;
  }
  pos += (size_t)tail_n;

  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open(prog, pos, "sparse.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  free(prog);
  check(s != NULL && ecode == 0, "tupu_open sparse");
  if (s == NULL) return;

  int in0_id = tupu_rel_id(s, "in0");
  int in99_id = tupu_rel_id(s, "in99");
  int out0_id = tupu_rel_id(s, "out0");
  int out99_id = tupu_rel_id(s, "out99");
  int edge_id = tupu_rel_id(s, "edge");
  int tc_id = tupu_rel_id(s, "tc");
  int ok_id = tupu_rel_id(s, "ok");
  check(in0_id >= 0 && in99_id >= 0 && out0_id >= 0 && out99_id >= 0 && edge_id >= 0 && tc_id >= 0 && ok_id >= 0,
        "sparse rel ids found");
  check(tupu_rel_id(s, "missing") == -1, "sparse missing rel returns -1");

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  long rss_f10 = -1, rss_f500 = -1;

  for (int frame = 1; frame <= 500; frame++) {
    check(tupu_reset(s) == 0, "sparse tupu_reset");
    uint32_t sym[6];
    for (int i = 0; i < 6; i++) {
      char sbuf[32];
      int sn = snprintf(sbuf, sizeof sbuf, "f%d_s%d", frame, i);
      check(sn > 0 && tupu_intern(s, sbuf, (size_t)sn, &sym[i]) == 0, "sparse tupu_intern");
    }
    int64_t r0[1] = { (int64_t)sym[0] };
    check(tupu_add_row(s, in0_id, r0, 1) == 0, "sparse add in0");
    for (int i = 0; i < 5; i++) {
      int64_t erow[2] = { (int64_t)sym[i], (int64_t)sym[i + 1] };
      check(tupu_add_row(s, edge_id, erow, 2) == 0, "sparse add edge");
    }
    check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "sparse tupu_eval");
    check(tupu_rel_count(s, out0_id) == 1, "sparse out0 == 1");
    check(tupu_rel_count(s, ok_id) == 1, "sparse ok == 1");
    check(tupu_rel_count(s, out99_id) == 0, "sparse out99 == 0");
    check(tupu_rel_count(s, tc_id) == 15, "sparse tc == 15");
    if (frame == 10) rss_f10 = read_rss_kb();
    if (frame == 500) rss_f500 = read_rss_kb();
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
  check(rss_f10 >= 0 && rss_f500 >= 0, "sparse rss readable");
#if !defined(__SANITIZE_ADDRESS__)
  check(rss_f500 <= rss_f10 + 4096, "sparse rss bounded");
#endif
  printf("sparse_500_frames_ms=%.2f rss_f10=%ldKB rss_f500=%ldKB\n", ms, rss_f10, rss_f500);
  tupu_close(s);
}

static const char *NEG_PROG =
    ".decl in(x: symbol)\n"
    ".input in\n"
    ".decl bad(x: symbol)\n"
    ".input bad\n"
    ".decl ok(x: symbol)\n"
    ".output ok\n"
    "ok(x) :- in(x), !bad(x).\n";

static void test_eval_requires_reset(void) {
  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open_ex(NEG_PROG, strlen(NEG_PROG), "neg.dl", 1000, 1, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "tupu_open_ex neg");
  if (s == NULL) return;

  int in_id = tupu_rel_id(s, "in");
  int bad_id = tupu_rel_id(s, "bad");
  int ok_id = tupu_rel_id(s, "ok");
  check(in_id >= 0 && bad_id >= 0 && ok_id >= 0, "neg rel ids found");

  ecode = -1;
  check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == -1 && ecode == TUPU_CODE_USAGE,
        "replay before eval fails closed with TUPU_CODE_USAGE");

  uint32_t sym_a = 0, sym_b = 0;
  check(tupu_intern(s, "a", 1, &sym_a) == 0, "intern a");
  int64_t row_a[1] = { (int64_t)sym_a };
  TupuI128 row_a128[1] = { tupu_i128_from_u64(sym_a) };
  check(tupu_add_row(s, in_id, row_a, 1) == 0, "add in(a)");
  ecode = -1;
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "eval neg first frame");
  check(tupu_rel_count(s, ok_id) == 1, "ok has 1 row");

  ecode = -1;
  check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "replay after eval succeeds");
  check(tupu_intern(s, "b", 1, &sym_b) == -1 && tupu_last_err_code(s) == TUPU_CODE_USAGE,
        "intern after eval fails closed");
  check(tupu_add_row(s, bad_id, row_a, 1) == -1 && tupu_last_err_code(s) == TUPU_CODE_USAGE,
        "add_row after eval fails closed");
  check(tupu_add_row_i128(s, bad_id, row_a128, 1) == -1 && tupu_last_err_code(s) == TUPU_CODE_USAGE,
        "add_row_i128 after eval fails closed");
  ecode = -1;
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == -1 && ecode == TUPU_CODE_USAGE,
        "eval after eval fails closed");

  check(tupu_reset(s) == 0, "reset after eval succeeds");
  check(tupu_intern(s, "a", 1, &sym_a) == 0, "re-intern a after reset");
  row_a[0] = (int64_t)sym_a;
  check(tupu_add_row(s, in_id, row_a, 1) == 0, "add in(a) after reset");
  check(tupu_add_row(s, bad_id, row_a, 1) == 0, "add bad(a) after reset");
  ecode = -1;
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "eval neg second frame");
  check(tupu_rel_count(s, ok_id) == 0, "ok has 0 rows after bad(a)");

  ecode = -1;
  TupuSession *budget_session = tupu_open_ex(TC_PROG, strlen(TC_PROG), "tc_budget.dl", 1, 1, 1, ebuf, sizeof ebuf, &ecode);
  check(budget_session != NULL && ecode == 0, "tupu_open_ex budget 1");
  if (budget_session != NULL) {
    int edge_id = tupu_rel_id(budget_session, "edge");
    int tc_id = tupu_rel_id(budget_session, "tc");
    check(edge_id >= 0 && tc_id >= 0, "budget rel ids found");
    uint32_t node_a = 0, node_b = 0, node_c = 0;
    check(tupu_intern(budget_session, "a", 1, &node_a) == 0, "budget intern a");
    check(tupu_intern(budget_session, "b", 1, &node_b) == 0, "budget intern b");
    check(tupu_intern(budget_session, "c", 1, &node_c) == 0, "budget intern c");
    int64_t edge_ab[2] = { (int64_t)node_a, (int64_t)node_b };
    int64_t edge_bc[2] = { (int64_t)node_b, (int64_t)node_c };
    check(tupu_add_row(budget_session, edge_id, edge_ab, 2) == 0, "budget add ab");
    check(tupu_add_row(budget_session, edge_id, edge_bc, 2) == 0, "budget add bc");
    ecode = -1;
    check(tupu_eval(budget_session, &ecode, ebuf, sizeof ebuf) == -1 && ecode == TUPU_CODE_DATALOG_BUDGET,
          "budget 1 eval fails with TUPU_CODE_DATALOG_BUDGET");
    check(tupu_add_row(budget_session, edge_id, edge_ab, 2) == -1 &&
              tupu_last_err_code(budget_session) == TUPU_CODE_USAGE,
          "add_row after failed eval fails closed");
    ecode = -1;
    check(tupu_eval(budget_session, &ecode, ebuf, sizeof ebuf) == -1 && ecode == TUPU_CODE_USAGE,
          "eval after failed eval fails closed");
    ecode = -1;
    check(tupu_replay(budget_session, &ecode, ebuf, sizeof ebuf) == -1 && ecode == TUPU_CODE_USAGE,
          "replay after failed eval fails closed");
    check(tupu_reset(budget_session) == 0, "reset after failed eval succeeds");
    check(tupu_intern(budget_session, "a", 1, &node_a) == 0, "budget re-intern a");
    check(tupu_intern(budget_session, "b", 1, &node_b) == 0, "budget re-intern b");
    edge_ab[0] = (int64_t)node_a;
    edge_ab[1] = (int64_t)node_b;
    check(tupu_add_row(budget_session, edge_id, edge_ab, 2) == 0, "budget add 1 row after reset");
    ecode = -1;
    check(tupu_eval(budget_session, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0,
          "budget 1 eval with 1 row succeeds after reset");
    check(tupu_rel_count(budget_session, tc_id) == 1, "budget 1 tc count == 1");
    tupu_close(budget_session);
  }
  tupu_close(s);
}

static void test_delta_not_displaced_and_empty_skip(void) {
  const char *prog =
      ".decl tag(t: symbol)\n"
      ".input tag\n"
      ".decl edge(x: symbol, y: symbol)\n"
      ".input edge\n"
      ".decl seed(x: symbol, y: symbol)\n"
      ".input seed\n"
      ".decl aux(x: symbol)\n"
      ".input aux\n"
      ".decl reach(x: symbol, y: symbol)\n"
      ".output reach\n"
      ".decl other(x: symbol, y: symbol)\n"
      ".output other\n"
      "reach(x, y) :- seed(x, y).\n"
      "reach(x, z) :- tag(t), edge(x, y), reach(y, z).\n"
      "reach(x, y) :- other(x, y).\n"
      "other(x, y) :- reach(x, y), aux(x).\n";
  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open_ex(prog, strlen(prog), "deltaskip.dl", 1000000, 1, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "tupu_open_ex deltaskip");
  if (s != NULL) {
    int tag_id = tupu_rel_id(s, "tag");
    int edge_id = tupu_rel_id(s, "edge");
    int seed_id = tupu_rel_id(s, "seed");
    int reach_id = tupu_rel_id(s, "reach");
    int other_id = tupu_rel_id(s, "other");
    check(tag_id >= 0 && edge_id >= 0 && seed_id >= 0 && reach_id >= 0 && other_id >= 0, "deltaskip rel ids");
    for (int i = 0; i < 200; i++) {
      char buf[16];
      int n = snprintf(buf, sizeof buf, "t%d", i);
      uint32_t sym = 0;
      check(n > 0 && tupu_intern(s, buf, (size_t)n, &sym) == 0, "deltaskip intern tag");
      int64_t row[1] = { (int64_t)sym };
      check(tupu_add_row(s, tag_id, row, 1) == 0, "deltaskip add tag");
    }
    uint32_t n0 = 0, n1 = 0, n2 = 0;
    check(tupu_intern(s, "n0", 2, &n0) == 0, "deltaskip intern n0");
    check(tupu_intern(s, "n1", 2, &n1) == 0, "deltaskip intern n1");
    check(tupu_intern(s, "n2", 2, &n2) == 0, "deltaskip intern n2");
    int64_t erow[2] = { (int64_t)n0, (int64_t)n1 };
    int64_t srow[2] = { (int64_t)n1, (int64_t)n2 };
    check(tupu_add_row(s, edge_id, erow, 2) == 0, "deltaskip add edge");
    check(tupu_add_row(s, seed_id, srow, 2) == 0, "deltaskip add seed");
    ecode = -1;
    check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "deltaskip eval");
    check(tupu_rel_count(s, other_id) == 0, "deltaskip other == 0");
    check(tupu_rel_count(s, reach_id) == 2, "deltaskip reach == 2");
    if (tupu_rel_count(s, reach_id) == 2) {
      Pair got[2];
      for (uint32_t r = 0; r < 2; r++) {
        int64_t v[2];
        check(tupu_rel_read_row(s, reach_id, r, v, 2) == 0, "deltaskip read reach");
        got[r].a = v[0];
        got[r].b = v[1];
      }
      qsort(got, 2, sizeof got[0], cmp_pair);
      check(got[0].a == (int64_t)n0 && got[0].b == (int64_t)n2, "deltaskip reach row 0");
      check(got[1].a == (int64_t)n1 && got[1].b == (int64_t)n2, "deltaskip reach row 1");
    }
    ecode = -1;
    check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "deltaskip replay");
    tupu_close(s);
  }

  const char *join_prog =
      ".decl pop(x: symbol)\n"
      ".input pop\n"
      ".decl emp(x: symbol)\n"
      ".input emp\n"
      ".decl out(x: symbol)\n"
      ".output out\n"
      "out(x) :- pop(x), emp(x).\n";
  ecode = -1;
  TupuSession *js = tupu_open(join_prog, strlen(join_prog), "empty_join.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  check(js != NULL && ecode == 0, "tupu_open empty_join");
  if (js != NULL) {
    int pop_id = tupu_rel_id(js, "pop");
    int out_id = tupu_rel_id(js, "out");
    check(pop_id >= 0 && out_id >= 0, "empty_join rel ids");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int frame = 1; frame <= 500; frame++) {
      check(tupu_reset(js) == 0, "empty_join reset");
      for (int i = 0; i < 200; i++) {
        char buf[16];
        int n = snprintf(buf, sizeof buf, "p%d", i);
        uint32_t sym = 0;
        check(n > 0 && tupu_intern(js, buf, (size_t)n, &sym) == 0, "empty_join intern");
        int64_t row[1] = { (int64_t)sym };
        check(tupu_add_row(js, pop_id, row, 1) == 0, "empty_join add pop");
      }
      ecode = -1;
      check(tupu_eval(js, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "empty_join eval");
      check(tupu_rel_count(js, out_id) == 0, "empty_join out == 0");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("delta_empty_500_frames_ms=%.2f\n", ms);
    tupu_close(js);
  }
}

static void test_replay_checks_all_idb_and_round_order(void) {
  const char *prog =
      ".decl edge(x: symbol, y: symbol)\n"
      ".input edge\n"
      ".decl mid(x: symbol, y: symbol)\n"
      ".decl out(x: symbol, y: symbol)\n"
      ".output out\n"
      "mid(x, y) :- edge(x, y).\n"
      "mid(x, z) :- mid(x, y), edge(y, z).\n"
      "out(x, y) :- mid(x, y).\n";
  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open_ex(prog, strlen(prog), "replay_idb.dl", 100000, 1, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "tupu_open_ex replay_idb");
  if (s == NULL) return;
  int edge_id = tupu_rel_id(s, "edge");
  int mid_id = tupu_rel_id(s, "mid");
  int out_id = tupu_rel_id(s, "out");
  check(edge_id >= 0 && mid_id >= 0 && out_id >= 0, "replay_idb rel ids");

  uint32_t sa = 0, sb = 0, sc = 0, sd = 0;
  check(tupu_intern(s, "a", 1, &sa) == 0 && tupu_intern(s, "b", 1, &sb) == 0 && tupu_intern(s, "c", 1, &sc) == 0,
        "replay_idb intern abc");
  int64_t e1[2] = { (int64_t)sa, (int64_t)sb };
  int64_t e2[2] = { (int64_t)sb, (int64_t)sc };
  check(tupu_add_row(s, edge_id, e1, 2) == 0 && tupu_add_row(s, edge_id, e2, 2) == 0, "replay_idb add edges");
  ecode = -1;
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "replay_idb eval");
  check(tupu_rel_count(s, mid_id) == 3 && tupu_rel_count(s, out_id) == 3, "replay_idb mid==3 out==3");
  if (tupu_rel_count(s, out_id) == 3) {
    Pair got[3];
    for (uint32_t r = 0; r < 3; r++) {
      int64_t v[2];
      check(tupu_rel_read_row(s, out_id, r, v, 2) == 0, "replay_idb read out");
      got[r].a = v[0];
      got[r].b = v[1];
    }
    qsort(got, 3, sizeof got[0], cmp_pair);
    check(got[0].a == (int64_t)sa && got[0].b == (int64_t)sb, "replay_idb row ab");
    check(got[1].a == (int64_t)sa && got[1].b == (int64_t)sc, "replay_idb row ac");
    check(got[2].a == (int64_t)sb && got[2].b == (int64_t)sc, "replay_idb row bc");
  }
  ecode = -1;
  check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "replay_idb tupu_replay f1");

  check(tupu_reset(s) == 0, "replay_idb reset");
  check(tupu_intern(s, "a", 1, &sa) == 0 && tupu_intern(s, "b", 1, &sb) == 0 &&
            tupu_intern(s, "c", 1, &sc) == 0 && tupu_intern(s, "d", 1, &sd) == 0,
        "replay_idb intern abcd");
  int64_t f1[2] = { (int64_t)sa, (int64_t)sb };
  int64_t f2[2] = { (int64_t)sb, (int64_t)sc };
  int64_t f3[2] = { (int64_t)sc, (int64_t)sd };
  check(tupu_add_row(s, edge_id, f1, 2) == 0 && tupu_add_row(s, edge_id, f2, 2) == 0 &&
            tupu_add_row(s, edge_id, f3, 2) == 0,
        "replay_idb add edges f2");
  ecode = -1;
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "replay_idb eval f2");
  check(tupu_rel_count(s, mid_id) == 6 && tupu_rel_count(s, out_id) == 6, "replay_idb f2 counts == 6");
  ecode = -1;
  check(tupu_replay(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "replay_idb tupu_replay f2");
  tupu_close(s);
}

static uint64_t shuffle_next(uint64_t *state) {
  uint64_t mix = (*state += 0x9e3779b97f4a7c15ULL);
  mix = (mix ^ (mix >> 30)) * 0xbf58476d1ce4e5b9ULL;
  mix = (mix ^ (mix >> 27)) * 0x94d049bb133111ebULL;
  return mix ^ (mix >> 31);
}

static void shuffle_indices(uint32_t *indices, size_t count, uint64_t *state) {
  if (count < 2) return;
  for (size_t pos = count - 1; pos > 0; pos--) {
    size_t swap_idx = (size_t)(shuffle_next(state) % (uint64_t)(pos + 1));
    uint32_t saved = indices[pos];
    indices[pos] = indices[swap_idx];
    indices[swap_idx] = saved;
  }
}

static const char *SHUFFLE_DECLS =
    ".decl baseBelow(lhs: symbol, rhs: symbol, diff: number)\n"
    ".input baseBelow\n"
    ".decl baseLeq(lhs: symbol, rhs: symbol, diff: number)\n"
    ".input baseLeq\n"
    ".decl belowOwed(owner: symbol, lhs: symbol, rhs: symbol, diff: number)\n"
    ".input belowOwed\n"
    ".decl alloc(slot: symbol, kind: symbol, point: symbol)\n"
    ".input alloc\n"
    ".decl dead(slot: symbol, point: symbol)\n"
    ".input dead\n"
    ".decl next(prev: symbol, curr: symbol)\n"
    ".input next\n"
    ".decl evalPoint(point: symbol)\n"
    ".input evalPoint\n"
    ".decl belowFrom(lhs: symbol, rhs: symbol, diff: number)\n"
    ".output belowFrom\n"
    ".decl leqFrom(lhs: symbol, rhs: symbol, diff: number)\n"
    ".output leqFrom\n"
    ".decl reuseCand(slot: symbol, point: symbol)\n"
    ".output reuseCand\n"
    ".decl reuseEarlier(slot: symbol, point: symbol)\n"
    ".output reuseEarlier\n"
    ".decl reuseLaterAlloc(slot: symbol, point: symbol)\n"
    ".output reuseLaterAlloc\n"
    ".decl reuseAt(slot: symbol, point: symbol)\n"
    ".output reuseAt\n"
    ".decl reject(owner: symbol, subject: symbol, val: number)\n"
    ".output reject\n";

static const char *const SHUFFLE_RULES[] = {
    "belowFrom(lhs, rhs, diff) :- baseBelow(lhs, rhs, diff).\n",
    "leqFrom(lhs, rhs, diff) :- baseLeq(lhs, rhs, diff).\n",
    "belowFrom(lhs, rhs, diff - 1) :- baseLeq(lhs, rhs, diff), "
    "diff >= -1000000000000000000000000000000, diff <= 1000000000000000000000000000000, ord(lhs) < ord(rhs).\n",
    "leqFrom(lhs, rhs, diff) :- belowFrom(lhs, rhs, diff).\n",
    "leqFrom(lhs, rhs, diff + 1) :- belowFrom(lhs, rhs, diff), "
    "diff >= -1000000000000000000000000000000, diff <= 1000000000000000000000000000000.\n",
    "belowFrom(lhs, rhs, lhs_diff + rhs_diff) :- belowFrom(lhs, mid, lhs_diff), leqFrom(mid, rhs, rhs_diff), "
    "ord(lhs) != ord(rhs), lhs_diff >= -1000000000000000000000000000000, lhs_diff <= 1000000000000000000000000000000, "
    "rhs_diff >= -1000000000000000000000000000000, rhs_diff <= 1000000000000000000000000000000.\n",
    "belowFrom(lhs, rhs, lhs_diff + rhs_diff) :- leqFrom(lhs, mid, lhs_diff), belowFrom(mid, rhs, rhs_diff), "
    "ord(lhs) != ord(rhs), lhs_diff >= -1000000000000000000000000000000, lhs_diff <= 1000000000000000000000000000000, "
    "rhs_diff >= -1000000000000000000000000000000, rhs_diff <= 1000000000000000000000000000000.\n",
    "leqFrom(lhs, rhs, lhs_diff + rhs_diff) :- leqFrom(lhs, mid, lhs_diff), leqFrom(mid, rhs, rhs_diff), "
    "ord(lhs) != ord(rhs), lhs_diff >= -1000000000000000000000000000000, lhs_diff <= 1000000000000000000000000000000, "
    "rhs_diff >= -1000000000000000000000000000000, rhs_diff <= 1000000000000000000000000000000.\n",
    "reuseCand(slot, point) :- dead(slot, point), evalPoint(point), alloc(slot, _, alloc_pt), "
    "belowFrom(alloc_pt, point, diff), diff >= 0.\n",
    "reuseCand(slot, point) :- dead(slot, prev_pt), next(prev_pt, point), evalPoint(point), alloc(slot, _, alloc_pt), "
    "leqFrom(alloc_pt, point, diff), diff >= 0.\n",
    "reuseEarlier(slot, point) :- reuseCand(slot, prev_pt), next(prev_pt, point), evalPoint(point).\n",
    "reuseEarlier(slot, point) :- reuseEarlier(slot, prev_pt), next(prev_pt, point), evalPoint(point).\n",
    "reuseLaterAlloc(slot, point) :- reuseCand(slot, point), alloc(slot, _, alloc_pt), alloc(other, _, alloc_pt), "
    "ord(other) < ord(slot), reuseCand(other, point), !reuseEarlier(other, point).\n",
    "reuseAt(slot, point) :- reuseCand(slot, point), !reuseEarlier(slot, point), !reuseLaterAlloc(slot, point).\n",
    "reject(owner, lhs, diff) :- belowOwed(owner, lhs, rhs, diff), !belowFrom(lhs, rhs, diff).\n",
    "reject(\"missing_reuse\", slot, ord(point)) :- dead(slot, point), evalPoint(point), "
    "alloc(slot, \"must_reuse\", _), !reuseAt(slot, point).\n",
};

typedef struct {
  const char *rel_name;
  const char *sym_cols[3];
  const char *num_text;
  int has_num;
} ShuffleFactRow;

static const ShuffleFactRow SHUFFLE_DENSE_FACTS[] = {
    {"baseBelow", {"pt_alpha", "pt_beta", NULL}, "1", 1},
    {"baseBelow", {"pt_beta", "pt_gamma", NULL}, "0", 1},
    {"baseBelow", {"pt_gamma", "pt_delta", NULL}, "2", 1},
    {"baseBelow", {"pt_delta", "pt_epsilon", NULL}, "-1", 1},
    {"baseBelow", {"pt_epsilon", "pt_zeta", NULL}, "3", 1},
    {"baseBelow", {"pt_alpha", "pt_eta", NULL}, "0", 1},
    {"baseBelow", {"pt_eta", "pt_theta", NULL}, "5", 1},
    {"baseBelow", {"edge_lo", "edge_mid", NULL}, "18446744073709551616", 1},
    {"baseBelow", {"limit_lo", "limit_hi", NULL}, "170141183460469231731687303715884105727", 1},

    {"baseLeq", {"pt_alpha", "pt_alpha", NULL}, "0", 1},
    {"baseLeq", {"pt_beta", "pt_beta", NULL}, "0", 1},
    {"baseLeq", {"pt_beta", "pt_delta", NULL}, "1", 1},
    {"baseLeq", {"pt_gamma", "pt_epsilon", NULL}, "-3", 1},
    {"baseLeq", {"pt_zeta", "pt_iota", NULL}, "4", 1},
    {"baseLeq", {"pt_theta", "pt_kappa", NULL}, "-5", 1},
    {"baseLeq", {"pt_iota", "pt_kappa", NULL}, "2", 1},
    {"baseLeq", {"edge_mid", "edge_hi", NULL}, "-18446744073709551616", 1},
    {"baseLeq", {"edge_min_src", "edge_min_dst", NULL}, "-170141183460469231731687303715884105728", 1},

    {"belowOwed", {"chk_alpha", "pt_alpha", "pt_gamma"}, "1", 1},
    {"belowOwed", {"chk_beta", "pt_alpha", "pt_delta"}, "99", 1},
    {"belowOwed", {"chk_gamma", "pt_delta", "pt_alpha"}, "-1", 1},
    {"belowOwed", {"chk_delta", "edge_lo", "edge_hi"}, "0", 1},
    {"belowOwed", {"chk_epsilon", "edge_lo", "edge_hi"}, "18446744073709551616", 1},
    {"belowOwed", {"chk_alpha", "limit_lo", "limit_hi"}, "170141183460469231731687303715884105727", 1},
    {"belowOwed", {"chk_beta", "edge_min_src", "edge_min_dst"}, "-170141183460469231731687303715884105728", 1},

    {"next", {"pt_alpha", "pt_beta", NULL}, NULL, 0},
    {"next", {"pt_beta", "pt_gamma", NULL}, NULL, 0},
    {"next", {"pt_gamma", "pt_delta", NULL}, NULL, 0},
    {"next", {"pt_delta", "pt_epsilon", NULL}, NULL, 0},
    {"next", {"pt_epsilon", "pt_zeta", NULL}, NULL, 0},
    {"next", {"pt_eta", "pt_theta", NULL}, NULL, 0},

    {"evalPoint", {"pt_beta", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_gamma", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_delta", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_epsilon", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_zeta", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_theta", NULL, NULL}, NULL, 0},

    {"alloc", {"slot_alpha", "free_slot", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_beta", "free_slot", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_gamma", "free_slot", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_delta", "must_reuse", "pt_beta"}, NULL, 0},
    {"alloc", {"slot_epsilon", "pinned", "pt_delta"}, NULL, 0},
    {"alloc", {"slot_zeta", "must_reuse", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_eta", "free_slot", "pt_eta"}, NULL, 0},

    {"dead", {"slot_alpha", "pt_gamma", NULL}, NULL, 0},
    {"dead", {"slot_beta", "pt_gamma", NULL}, NULL, 0},
    {"dead", {"slot_gamma", "pt_delta", NULL}, NULL, 0},
    {"dead", {"slot_delta", "pt_delta", NULL}, NULL, 0},
    {"dead", {"slot_epsilon", "pt_epsilon", NULL}, NULL, 0},
    {"dead", {"slot_zeta", "pt_gamma", NULL}, NULL, 0},
    {"dead", {"slot_eta", "pt_theta", NULL}, NULL, 0},
};

static const ShuffleFactRow SHUFFLE_SPARSE_FACTS[] = {
    {"baseBelow", {"pt_alpha", "pt_beta", NULL}, "2", 1},
    {"baseBelow", {"pt_beta", "pt_gamma", NULL}, "1", 1},
    {"baseBelow", {"limit_lo", "limit_hi", NULL}, "170141183460469231731687303715884105727", 1},

    {"baseLeq", {"pt_gamma", "pt_delta", NULL}, "0", 1},
    {"baseLeq", {"edge_min_src", "edge_min_dst", NULL}, "-170141183460469231731687303715884105728", 1},

    {"belowOwed", {"chk_alpha", "pt_alpha", "pt_gamma"}, "3", 1},
    {"belowOwed", {"chk_beta", "pt_alpha", "pt_delta"}, "10", 1},

    {"next", {"pt_beta", "pt_gamma", NULL}, NULL, 0},
    {"next", {"pt_gamma", "pt_delta", NULL}, NULL, 0},

    {"evalPoint", {"pt_beta", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_gamma", NULL, NULL}, NULL, 0},
    {"evalPoint", {"pt_delta", NULL, NULL}, NULL, 0},

    {"alloc", {"slot_alpha", "free_slot", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_beta", "free_slot", "pt_alpha"}, NULL, 0},
    {"alloc", {"slot_gamma", "must_reuse", "pt_alpha"}, NULL, 0},

    {"dead", {"slot_alpha", "pt_beta", NULL}, NULL, 0},
    {"dead", {"slot_beta", "pt_beta", NULL}, NULL, 0},
    {"dead", {"slot_gamma", "pt_gamma", NULL}, NULL, 0},
};

typedef struct {
  char *bytes;
  size_t len;
  size_t cap;
  uint32_t rel_counts[7];
} ShuffleSnapshot;

static void shuffle_append_bytes(ShuffleSnapshot *snap, const char *data, size_t len) {
  if (len == 0) return;
  if (snap->len + len + 1 > snap->cap) {
    size_t next_cap = snap->cap == 0 ? 4096 : snap->cap;
    while (next_cap < snap->len + len + 1) next_cap *= 2;
    char *grown = realloc(snap->bytes, next_cap);
    check(grown != NULL, "shuffle_append_bytes realloc");
    if (grown == NULL) return;
    snap->bytes = grown;
    snap->cap = next_cap;
  }
  memcpy(snap->bytes + snap->len, data, len);
  snap->len += len;
  snap->bytes[snap->len] = '\0';
}

static int cmp_cstr_ptr(const void *left_ptr, const void *right_ptr) {
  const char *const *lhs = left_ptr;
  const char *const *rhs = right_ptr;
  return strcmp(*lhs, *rhs);
}

static void shuffle_collect_snapshot(const TupuSession *session, const char *const *rel_names, size_t rel_count,
                                     ShuffleSnapshot *snap) {
  memset(snap, 0, sizeof *snap);
  for (size_t rel_idx = 0; rel_idx < rel_count; rel_idx++) {
    int rel_id = tupu_rel_id(session, rel_names[rel_idx]);
    int ncols = tupu_rel_ncols(session, rel_id);
    check(rel_id >= 0 && ncols > 0 && ncols <= 4, "shuffle output rel metadata");
    if (rel_id < 0 || ncols <= 0 || ncols > 4) continue;
    uint32_t row_count = tupu_rel_count(session, rel_id);
    snap->rel_counts[rel_idx] = row_count;
    char header_buf[128];
    int header_len = snprintf(header_buf, sizeof header_buf, "@rel %s count=%u\n", rel_names[rel_idx], row_count);
    check(header_len > 0 && (size_t)header_len < sizeof header_buf, "shuffle format header");
    if (header_len > 0) shuffle_append_bytes(snap, header_buf, (size_t)header_len);
    if (row_count == 0) continue;

    char **row_strs = calloc(row_count, sizeof *row_strs);
    check(row_strs != NULL, "shuffle alloc row_strs");
    if (row_strs == NULL) continue;

    for (uint32_t row_idx = 0; row_idx < row_count; row_idx++) {
      TupuI128 wide_vals[4];
      check(tupu_rel_read_row_i128(session, rel_id, row_idx, wide_vals, ncols) == 0, "shuffle read row");
      char line_buf[512];
      size_t line_pos = 0;
      int prefix_len = snprintf(line_buf, sizeof line_buf, "%s", rel_names[rel_idx]);
      check(prefix_len > 0 && (size_t)prefix_len < sizeof line_buf, "shuffle line prefix");
      if (prefix_len > 0) line_pos = (size_t)prefix_len;

      for (int col_idx = 0; col_idx < ncols; col_idx++) {
        check(line_pos + 2 < sizeof line_buf, "shuffle line tab room");
        if (line_pos + 2 >= sizeof line_buf) break;
        line_buf[line_pos++] = '\t';
        int kind = tupu_rel_col_kind(session, rel_id, col_idx);
        if (kind == TUPU_COL_SYMBOL) {
          size_t sym_len = 0;
          const char *sym_ptr = tupu_sym_str(session, (uint32_t)wide_vals[col_idx].lo, &sym_len);
          check(sym_ptr != NULL && line_pos + sym_len + 2 < sizeof line_buf, "shuffle sym lookup");
          if (sym_ptr != NULL && line_pos + sym_len + 2 < sizeof line_buf) {
            memcpy(line_buf + line_pos, sym_ptr, sym_len);
            line_pos += sym_len;
          }
        } else {
          check(kind == TUPU_COL_NUMBER, "shuffle col kind number");
          char num_buf[TUPU_I128_TEXT_MAX];
          size_t num_len = tupu_i128_format(wide_vals[col_idx], num_buf);
          check(num_len > 0 && line_pos + num_len + 2 < sizeof line_buf, "shuffle num format");
          if (line_pos + num_len + 2 < sizeof line_buf) {
            memcpy(line_buf + line_pos, num_buf, num_len);
            line_pos += num_len;
          }
        }
      }
      line_buf[line_pos++] = '\n';
      line_buf[line_pos] = '\0';
      row_strs[row_idx] = malloc(line_pos + 1);
      check(row_strs[row_idx] != NULL, "shuffle alloc row string");
      if (row_strs[row_idx] != NULL) memcpy(row_strs[row_idx], line_buf, line_pos + 1);
    }

    qsort(row_strs, row_count, sizeof *row_strs, cmp_cstr_ptr);
    for (uint32_t row_idx = 0; row_idx < row_count; row_idx++) {
      if (row_strs[row_idx] != NULL) {
        shuffle_append_bytes(snap, row_strs[row_idx], strlen(row_strs[row_idx]));
        free(row_strs[row_idx]);
      }
    }
    free(row_strs);
  }
}

static int shuffle_sym_index(const char *const *syms, size_t sym_count, const char *name) {
  for (size_t idx = 0; idx < sym_count; idx++) {
    if (strcmp(syms[idx], name) == 0) return (int)idx;
  }
  return -1;
}

static int shuffle_is_narrow_num(TupuI128 wide_val) {
  if (wide_val.hi == 0 && wide_val.lo <= (uint64_t)INT64_MAX) return 1;
  if (wide_val.hi == -1 && wide_val.lo >= (uint64_t)INT64_MIN) return 1;
  return 0;
}

static void shuffle_run_frame(TupuSession *session, const ShuffleFactRow *facts, size_t fact_count, uint64_t fact_seed,
                              const char *const *rel_names, size_t rel_count, ShuffleSnapshot *snap) {
  check(tupu_reset(session) == 0, "shuffle tupu_reset");

  const char *sym_list[64];
  size_t sym_count = 0;
  for (size_t fact_idx = 0; fact_idx < fact_count; fact_idx++) {
    for (int col_idx = 0; col_idx < 3; col_idx++) {
      const char *sym_name = facts[fact_idx].sym_cols[col_idx];
      if (sym_name != NULL && shuffle_sym_index(sym_list, sym_count, sym_name) < 0) {
        check(sym_count < 64, "shuffle sym_list capacity");
        if (sym_count < 64) sym_list[sym_count++] = sym_name;
      }
    }
  }

  uint32_t sym_order[64];
  uint32_t sym_ids[64];
  for (size_t idx = 0; idx < sym_count; idx++) sym_order[idx] = (uint32_t)idx;

  uint32_t row_order[64];
  check(fact_count <= 64, "shuffle fact_count capacity");
  for (size_t idx = 0; idx < fact_count; idx++) row_order[idx] = (uint32_t)idx;

  if (fact_seed != 0) {
    uint64_t rng_state = fact_seed;
    shuffle_indices(sym_order, sym_count, &rng_state);
    shuffle_indices(row_order, fact_count, &rng_state);
  }

  for (size_t idx = 0; idx < sym_count; idx++) {
    uint32_t sym_slot = sym_order[idx];
    const char *sym_name = sym_list[sym_slot];
    check(tupu_intern(session, sym_name, strlen(sym_name), &sym_ids[sym_slot]) == 0, "shuffle tupu_intern");
  }

  for (size_t idx = 0; idx < fact_count; idx++) {
    const ShuffleFactRow *row = &facts[row_order[idx]];
    int rel_id = tupu_rel_id(session, row->rel_name);
    int ncols = tupu_rel_ncols(session, rel_id);
    check(rel_id >= 0 && ncols > 0 && ncols <= 4, "shuffle input rel metadata");
    if (rel_id < 0 || ncols <= 0 || ncols > 4) continue;

    int sym_limit = row->has_num ? ncols - 1 : ncols;
    TupuI128 parsed_num = {0, 0};
    int use_narrow = 1;
    if (row->has_num) {
      check(tupu_i128_parse(row->num_text, strlen(row->num_text), &parsed_num) == 0, "shuffle i128 parse");
      use_narrow = shuffle_is_narrow_num(parsed_num);
    }

    if (use_narrow) {
      int64_t row_vals[4];
      for (int col_idx = 0; col_idx < sym_limit; col_idx++) {
        int sym_slot = shuffle_sym_index(sym_list, sym_count, row->sym_cols[col_idx]);
        check(sym_slot >= 0, "shuffle sym index valid");
        row_vals[col_idx] = sym_slot >= 0 ? (int64_t)sym_ids[sym_slot] : 0;
      }
      if (row->has_num) row_vals[sym_limit] = (int64_t)parsed_num.lo;
      check(tupu_add_row(session, rel_id, row_vals, ncols) == 0, "shuffle tupu_add_row");
    } else {
      TupuI128 wide_vals[4];
      for (int col_idx = 0; col_idx < sym_limit; col_idx++) {
        int sym_slot = shuffle_sym_index(sym_list, sym_count, row->sym_cols[col_idx]);
        check(sym_slot >= 0, "shuffle wide sym index valid");
        wide_vals[col_idx] = tupu_i128_from_u64(sym_slot >= 0 ? sym_ids[sym_slot] : 0);
      }
      wide_vals[sym_limit] = parsed_num;
      check(tupu_add_row_i128(session, rel_id, wide_vals, ncols) == 0, "shuffle tupu_add_row_i128");
    }
  }

  char err_buf[256];
  int err_code = -1;
  int eval_rc = tupu_eval(session, &err_code, err_buf, sizeof err_buf);
  check(eval_rc == 0 && err_code == 0, "shuffle tupu_eval");
  if (eval_rc != 0) fprintf(stderr, "shuffle tupu_eval err=%s\n", err_buf);

  err_code = -1;
  int replay_rc = tupu_replay(session, &err_code, err_buf, sizeof err_buf);
  check(replay_rc == 0 && err_code == 0, "shuffle tupu_replay");
  if (replay_rc != 0) fprintf(stderr, "shuffle tupu_replay err=%s\n", err_buf);

  shuffle_collect_snapshot(session, rel_names, rel_count, snap);
}

static void test_shuffle_golden_order_independence(void) {
  static const char *const out_rels[] = {
      "belowFrom", "leqFrom", "reuseCand", "reuseEarlier", "reuseLaterAlloc", "reuseAt", "reject",
  };
  const size_t out_rel_count = sizeof out_rels / sizeof out_rels[0];
  const size_t rule_count = sizeof SHUFFLE_RULES / sizeof SHUFFLE_RULES[0];
  const size_t dense_count = sizeof SHUFFLE_DENSE_FACTS / sizeof SHUFFLE_DENSE_FACTS[0];
  const size_t sparse_count = sizeof SHUFFLE_SPARSE_FACTS / sizeof SHUFFLE_SPARSE_FACTS[0];

  size_t prog_cap = strlen(SHUFFLE_DECLS) + 1;
  for (size_t idx = 0; idx < rule_count; idx++) prog_cap += strlen(SHUFFLE_RULES[idx]);
  char *prog_buf = malloc(prog_cap);
  check(prog_buf != NULL, "shuffle alloc prog_buf");
  if (prog_buf == NULL) return;

  static const uint64_t rule_seeds[] = {0, 1, 2, 3};
  static const uint64_t fact_seeds[] = {0, 101, 202, 303};
  static const size_t thread_counts[] = {1, 4};

  ShuffleSnapshot canonical_dense;
  ShuffleSnapshot canonical_sparse;
  memset(&canonical_dense, 0, sizeof canonical_dense);
  memset(&canonical_sparse, 0, sizeof canonical_sparse);
  int has_dense = 0;
  int has_sparse = 0;

  for (size_t rule_seed_idx = 0; rule_seed_idx < sizeof rule_seeds / sizeof rule_seeds[0]; rule_seed_idx++) {
    uint32_t rule_perm[32];
    check(rule_count <= 32, "shuffle rule_perm capacity");
    for (size_t idx = 0; idx < rule_count; idx++) rule_perm[idx] = (uint32_t)idx;
    if (rule_seeds[rule_seed_idx] != 0) {
      uint64_t rule_state = rule_seeds[rule_seed_idx];
      shuffle_indices(rule_perm, rule_count, &rule_state);
    }

    size_t prog_pos = strlen(SHUFFLE_DECLS);
    memcpy(prog_buf, SHUFFLE_DECLS, prog_pos);
    for (size_t idx = 0; idx < rule_count; idx++) {
      const char *rule_text = SHUFFLE_RULES[rule_perm[idx]];
      size_t rule_len = strlen(rule_text);
      memcpy(prog_buf + prog_pos, rule_text, rule_len);
      prog_pos += rule_len;
    }
    prog_buf[prog_pos] = '\0';

    for (size_t thread_idx = 0; thread_idx < sizeof thread_counts / sizeof thread_counts[0]; thread_idx++) {
      size_t nthreads = thread_counts[thread_idx];
      char err_buf[256];
      int err_code = -1;
      TupuSession *session = tupu_open_ex(prog_buf, prog_pos, "shuffle_golden.dl", 1000000, 1, nthreads,
                                          err_buf, sizeof err_buf, &err_code);
      check(session != NULL && err_code == 0, "shuffle tupu_open_ex");
      if (session == NULL) {
        fprintf(stderr, "shuffle tupu_open_ex err=%s\n", err_buf);
        continue;
      }

      for (size_t fact_idx = 0; fact_idx < sizeof fact_seeds / sizeof fact_seeds[0]; fact_idx++) {
        uint64_t fact_seed = fact_seeds[fact_idx];
        ShuffleSnapshot dense_snap;
        ShuffleSnapshot sparse_snap;
        shuffle_run_frame(session, SHUFFLE_DENSE_FACTS, dense_count, fact_seed, out_rels, out_rel_count, &dense_snap);
        shuffle_run_frame(session, SHUFFLE_SPARSE_FACTS, sparse_count, fact_seed, out_rels, out_rel_count, &sparse_snap);

        check(dense_snap.rel_counts[0] > 0, "shuffle dense belowFrom non-empty");
        check(dense_snap.rel_counts[1] > 0, "shuffle dense leqFrom non-empty");
        check(dense_snap.rel_counts[2] > 0, "shuffle dense reuseCand non-empty");
        check(dense_snap.rel_counts[3] > 0, "shuffle dense reuseEarlier non-empty");
        check(dense_snap.rel_counts[4] > 0, "shuffle dense reuseLaterAlloc non-empty");
        check(dense_snap.rel_counts[5] > 0, "shuffle dense reuseAt non-empty");
        check(dense_snap.rel_counts[6] > 0, "shuffle dense reject non-empty");

        check(sparse_snap.rel_counts[0] > 0, "shuffle sparse belowFrom non-empty");
        check(sparse_snap.rel_counts[5] > 0, "shuffle sparse reuseAt non-empty");
        check(sparse_snap.rel_counts[6] > 0, "shuffle sparse reject non-empty");
        check(sparse_snap.rel_counts[0] < dense_snap.rel_counts[0], "shuffle sparse belowFrom distinct from dense");
        check(sparse_snap.rel_counts[5] < dense_snap.rel_counts[5], "shuffle sparse reuseAt distinct from dense");
        check(sparse_snap.rel_counts[6] < dense_snap.rel_counts[6], "shuffle sparse reject distinct from dense");

        if (!has_dense) {
          canonical_dense = dense_snap;
          has_dense = 1;
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "belowFrom\tedge_lo\tedge_hi\t0\n") != NULL,
                "shuffle canonical dense 128-bit transitive bound");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes,
                           "belowFrom\tlimit_lo\tlimit_hi\t170141183460469231731687303715884105727\n") != NULL,
                "shuffle canonical dense i128 max bound");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reject\tchk_beta\tpt_alpha\t99\n") != NULL,
                "shuffle canonical dense reject missing bound");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reject\tchk_gamma\tpt_delta\t-1\n") != NULL,
                "shuffle canonical dense reject negative bound");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes,
                           "reject\tchk_beta\tedge_min_src\t-170141183460469231731687303715884105728\n") != NULL,
                "shuffle canonical dense reject i128 min bound");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reject\tchk_alpha\t") == NULL &&
                    strstr(canonical_dense.bytes, "reject\tchk_delta\t") == NULL,
                "shuffle canonical dense satisfied belowOwed not rejected");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reuseLaterAlloc\tslot_beta\tpt_gamma\n") != NULL &&
                    strstr(canonical_dense.bytes, "reuseAt\tslot_alpha\tpt_gamma\n") != NULL &&
                    strstr(canonical_dense.bytes, "reuseAt\tslot_beta\tpt_gamma\n") == NULL,
                "shuffle canonical dense concurrent alloc tie-break");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reuseAt\tslot_alpha\tpt_delta\n") == NULL &&
                    strstr(canonical_dense.bytes, "reuseAt\tslot_gamma\tpt_delta\n") != NULL,
                "shuffle canonical dense reuseEarlier blocks slot_alpha not slot_gamma");
          check(canonical_dense.bytes != NULL &&
                    strstr(canonical_dense.bytes, "reject\tmissing_reuse\tslot_zeta\t") != NULL,
                "shuffle canonical dense missing_reuse reject");
        } else {
          int same_dense = dense_snap.len == canonical_dense.len &&
                           memcmp(dense_snap.bytes, canonical_dense.bytes, canonical_dense.len) == 0;
          check(same_dense, "shuffle dense snapshot matches canonical");
          free(dense_snap.bytes);
        }

        if (!has_sparse) {
          canonical_sparse = sparse_snap;
          has_sparse = 1;
          check(canonical_sparse.bytes != NULL &&
                    strstr(canonical_sparse.bytes, "reject\tchk_beta\tpt_alpha\t10\n") != NULL &&
                    strstr(canonical_sparse.bytes, "reject\tchk_alpha\t") == NULL,
                "shuffle canonical sparse reject exactness");
        } else {
          int same_sparse = sparse_snap.len == canonical_sparse.len &&
                            memcmp(sparse_snap.bytes, canonical_sparse.bytes, canonical_sparse.len) == 0;
          check(same_sparse, "shuffle sparse snapshot matches canonical");
          free(sparse_snap.bytes);
        }
      }
      tupu_close(session);
    }
  }

  free(canonical_dense.bytes);
  free(canonical_sparse.bytes);
  free(prog_buf);
}

static void test_ord_and_sparse_bitset_across_resets(void) {
  static const char *PROG =
      ".decl node(x: symbol)\n"
      ".input node\n"
      ".decl edge(x: symbol, y: symbol)\n"
      ".input edge\n"
      ".decl ord_lt(x: symbol, y: symbol)\n"
      ".output ord_lt\n"
      ".decl ord_b0(x: symbol)\n"
      ".output ord_b0\n"
      ".decl r0(x: symbol, y: symbol)\n"
      ".output r0\n"
      ".decl r1(x: symbol, y: symbol)\n"
      ".decl r2(x: symbol, y: symbol)\n"
      ".decl r3(x: symbol, y: symbol)\n"
      ".decl r4(x: symbol, y: symbol)\n"
      "node(\"b\").\n"
      "node(\"d\").\n"
      "node(\"f\").\n"
      "ord_lt(x, y) :- node(x), node(y), ord(x) < ord(y).\n"
      "ord_b0(x) :- node(x), ord(x) = 0.\n"
      "r0(x, y) :- edge(x, y).\n"
      "r0(x, z) :- r0(x, y), edge(y, z).\n"
      "r1(x, y) :- r2(x, y).\n"
      "r3(x, y) :- r4(x, y).\n";

  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open(PROG, strlen(PROG), "ord_sparse.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "ord_sparse open");
  if (s == NULL) return;

  int node_id = tupu_rel_id(s, "node");
  int edge_id = tupu_rel_id(s, "edge");
  int ord_lt_id = tupu_rel_id(s, "ord_lt");
  int ord_b0_id = tupu_rel_id(s, "ord_b0");
  int r0_id = tupu_rel_id(s, "r0");

  /* Frame 1: base symbols only */
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "ord_sparse f1 eval");
  check(tupu_rel_count(s, ord_lt_id) == 3, "ord_sparse f1 ord_lt == 3");
  check(tupu_rel_count(s, ord_b0_id) == 1, "ord_sparse f1 ord_b0 == 1");
  check(tupu_rel_count(s, r0_id) == 0, "ord_sparse f1 r0 == 0");

  /* Frame 2: interleaved eval symbols + populated fast2 r0 */
  check(tupu_reset(s) == 0, "ord_sparse f2 reset");
  uint32_t sa = 0, sc = 0, se = 0;
  check(tupu_intern(s, "a", 1, &sa) == 0, "intern a");
  check(tupu_intern(s, "c", 1, &sc) == 0, "intern c");
  check(tupu_intern(s, "e", 1, &se) == 0, "intern e");
  int64_t n1[1] = { (int64_t)sa }, n2[1] = { (int64_t)sc }, n3[1] = { (int64_t)se };
  check(tupu_add_row(s, node_id, n1, 1) == 0, "add node a");
  check(tupu_add_row(s, node_id, n2, 1) == 0, "add node c");
  check(tupu_add_row(s, node_id, n3, 1) == 0, "add node e");
  int64_t e1[2] = { (int64_t)sa, (int64_t)sc }, e2[2] = { (int64_t)sc, (int64_t)se };
  check(tupu_add_row(s, edge_id, e1, 2) == 0, "add edge ac");
  check(tupu_add_row(s, edge_id, e2, 2) == 0, "add edge ce");
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "ord_sparse f2 eval");
  check(tupu_rel_count(s, ord_lt_id) == 15, "ord_sparse f2 ord_lt == 15");
  check(tupu_rel_count(s, r0_id) == 3, "ord_sparse f2 r0 == 3");

  /* Frame 3: back to base symbols only */
  check(tupu_reset(s) == 0, "ord_sparse f3 reset");
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "ord_sparse f3 eval");
  check(tupu_rel_count(s, ord_lt_id) == 3, "ord_sparse f3 ord_lt == 3");
  check(tupu_rel_count(s, ord_b0_id) == 1, "ord_sparse f3 ord_b0 == 1");
  check(tupu_rel_count(s, r0_id) == 0, "ord_sparse f3 r0 == 0");

  /* Frame 4: >8192 symbols threshold crossing */
  check(tupu_reset(s) == 0, "ord_sparse f4 reset");
  uint32_t sfirst = 0, slast = 0, sb = 0;
  for (int i = 0; i < 8200; i++) {
    char buf[16];
    int n = snprintf(buf, sizeof buf, "s%d", i);
    uint32_t tmp = 0;
    check(tupu_intern(s, buf, (size_t)n, &tmp) == 0, "intern >8192");
    if (i == 0) sfirst = tmp;
    if (i == 8199) slast = tmp;
  }
  check(tupu_intern(s, "b", 1, &sb) == 0, "intern b");
  int64_t e3[2] = { (int64_t)sfirst, (int64_t)slast }, e4[2] = { (int64_t)slast, (int64_t)sb };
  check(tupu_add_row(s, edge_id, e3, 2) == 0, "add edge s0->s8199");
  check(tupu_add_row(s, edge_id, e4, 2) == 0, "add edge s8199->b");
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "ord_sparse f4 eval");
  check(tupu_rel_count(s, ord_lt_id) == 3, "ord_sparse f4 ord_lt == 3");
  check(tupu_rel_count(s, r0_id) == 3, "ord_sparse f4 r0 == 3");

  tupu_close(s);
}

static void test_lazy_bitset_bounds_check(void) {
  static const char *PROG =
      ".decl edge(x: symbol, y: symbol)\n"
      ".input edge\n"
      ".decl reach(x: symbol, y: symbol)\n"
      ".input reach\n"
      ".output reach\n"
      ".decl rev_reach(y: symbol, x: symbol)\n"
      ".output rev_reach\n"
      "reach(x, z) :- reach(x, y), edge(y, z).\n"
      "rev_reach(z, x) :- reach(x, y), edge(y, z).\n";

  char ebuf[256];
  int ecode = -1;
  TupuSession *s = tupu_open(PROG, strlen(PROG), "lazy_bitset_oob.dl", 1000000, 1, ebuf, sizeof ebuf, &ecode);
  check(s != NULL && ecode == 0, "lazy_bitset open");
  if (s == NULL) return;

  int edge_id = tupu_rel_id(s, "edge");
  int reach_id = tupu_rel_id(s, "reach");
  int rev_id = tupu_rel_id(s, "rev_reach");

  uint32_t syms[400];
  for (int i = 0; i < 400; i++) {
    char buf[16];
    int n = snprintf(buf, sizeof buf, "p%d", i);
    check(tupu_intern(s, buf, (size_t)n, &syms[i]) == 0, "lazy_bitset intern");
  }
  int64_t r0[2] = { (int64_t)syms[0], (int64_t)syms[1] };
  int64_t e0[2] = { (int64_t)syms[1], (int64_t)syms[350] };
  check(tupu_add_row(s, reach_id, r0, 2) == 0, "lazy_bitset add reach");
  check(tupu_add_row(s, edge_id, e0, 2) == 0, "lazy_bitset add edge");
  check(tupu_eval(s, &ecode, ebuf, sizeof ebuf) == 0 && ecode == 0, "lazy_bitset eval");
  check(tupu_rel_count(s, reach_id) == 2, "lazy_bitset reach == 2");
  check(tupu_rel_count(s, rev_id) == 1, "lazy_bitset rev_reach == 1");
  tupu_close(s);
}

int main(void) {
  test_1000_frames();
  test_budget_exceeded();
  test_arith_overflow();
  test_threads_equal();
  test_i128_and_replay();
  test_column_kinds();
  test_i128_text();
  test_sparse_multistratum_frames();
  test_eval_requires_reset();
  test_delta_not_displaced_and_empty_skip();
  test_replay_checks_all_idb_and_round_order();
  test_shuffle_golden_order_independence();
  test_ord_and_sparse_bitset_across_resets();
  test_lazy_bitset_bounds_check();
  printf("api_test failures=%d\n", fails);
  return fails == 0 ? 0 : 1;
}
