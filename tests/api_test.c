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

  for (int frame = 1; frame <= 1000; frame++) {
    check(tupu_reset(s) == 0, "tupu_reset");
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

int main(void) {
  test_1000_frames();
  test_budget_exceeded();
  test_arith_overflow();
  test_threads_equal();
  test_i128_and_replay();
  printf("api_test failures=%d\n", fails);
  return fails == 0 ? 0 : 1;
}
