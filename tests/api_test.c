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
  printf("api_test failures=%d\n", fails);
  return fails == 0 ? 0 : 1;
}
