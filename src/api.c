/* In-process C library API implementation (include/tupu.h). */
#include "../include/tupu.h"
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TupuSession {
  Program prog;
  Engine eng;
  Err err;
  char *src_copy;
  uint32_t ranked_nsyms;
};

static void report_err(const Err *e, int *err_code, char *err_buf, size_t err_cap) {
  if (err_code != NULL) *err_code = (int)e->code;
  if (err_buf != NULL && err_cap > 0) {
    if (e->file == NULL) (void)snprintf(err_buf, err_cap, "%s: %s", code_name(e->code), e->text);
    else (void)snprintf(err_buf, err_cap, "%s %s:%ld: %s", code_name(e->code), e->file, e->line, e->text);
  }
}

static I128 i128_from_tupu(TupuI128 v) {
  U128 u = ((U128)(uint64_t)v.hi << 64) | (U128)v.lo;
  return (I128)u;
}

static TupuI128 tupu_from_i128(I128 v) {
  U128 u = (U128)v;
  TupuI128 r;
  r.lo = (uint64_t)u;
  r.hi = (int64_t)(uint64_t)(u >> 64);
  return r;
}

TupuSession *tupu_open_ex(const char *src, size_t len, const char *name, uint64_t budget, int prov, size_t nthreads,
                          char *err_buf, size_t err_cap, int *err_code) {
  if (err_code != NULL) *err_code = 0;
  if (err_buf != NULL && err_cap > 0) err_buf[0] = '\0';
  if (src == NULL && len > 0) {
    Err e;
    (void)err_set(&e, CODE_USAGE, NULL, 0, "null program source");
    report_err(&e, err_code, err_buf, err_cap);
    return NULL;
  }
  TupuSession *s = calloc(1, sizeof *s);
  if (s == NULL) {
    Err e;
    (void)err_oom(&e);
    report_err(&e, err_code, err_buf, err_cap);
    return NULL;
  }
  s->src_copy = malloc(len + 1);
  if (s->src_copy == NULL) {
    (void)err_oom(&s->err);
    report_err(&s->err, err_code, err_buf, err_cap);
    free(s);
    return NULL;
  }
  if (len > 0) memcpy(s->src_copy, src, len);
  s->src_copy[len] = '\0';
  program_init(&s->prog);
  bool eng_open = false;
  Status st = parse_program(&s->prog, s->src_copy, len, name != NULL ? name : "<memory>", &s->err);
  if (st == STATUS_OK) st = check_program(&s->prog, &s->err);
  if (st == STATUS_OK) {
    st = engine_open(&s->eng, &s->prog, NULL, budget, prov != 0, nthreads, &s->err);
    eng_open = true;
  }
  if (st != STATUS_OK) {
    report_err(&s->err, err_code, err_buf, err_cap);
    if (eng_open) engine_close(&s->eng);
    program_free(&s->prog);
    free(s->src_copy);
    free(s);
    return NULL;
  }
  s->ranked_nsyms = s->prog.syms.n;
  sym_seal(&s->prog.syms); /* the rule program and its facts own these symbols; units come and go */
  return s;
}

TupuSession *tupu_open(const char *src, size_t len, const char *name, uint64_t budget, size_t nthreads,
                       char *err_buf, size_t err_cap, int *err_code) {
  return tupu_open_ex(src, len, name, budget, 0, nthreads, err_buf, err_cap, err_code);
}

int tupu_last_err_code(const TupuSession *s) {
  if (s == NULL) return -1;
  return (int)s->err.code;
}

void tupu_close(TupuSession *s) {
  if (s == NULL) return;
  engine_close(&s->eng);
  program_free(&s->prog);
  free(s->src_copy);
  free(s);
}

int tupu_reset(TupuSession *s) {
  if (s == NULL) return -1;
  if (engine_reset(&s->eng, &s->err) != STATUS_OK) return -1;
  /* the tables are empty, so no row names an evaluation symbol: drop them, and rank again on the next eval */
  sym_forget_evaluation(&s->prog.syms);
  s->ranked_nsyms = UINT32_MAX;
  return 0;
}

int tupu_rel_id(const TupuSession *s, const char *name) {
  if (s == NULL || name == NULL) return -1;
  size_t n = strlen(name);
  for (size_t i = 0; i < s->prog.rels.n; i++) {
    if (s->prog.rels.p[i].name.n == n && memcmp(s->prog.rels.p[i].name.s, name, n) == 0) return (int)i;
  }
  return -1;
}

int tupu_rel_ncols(const TupuSession *s, int rel_id) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  return s->prog.rels.p[rel_id].ncols;
}

int tupu_rel_col_kind(const TupuSession *s, int rel_id, int col) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  const Relation *r = &s->prog.rels.p[rel_id];
  if (col < 0 || col >= r->ncols) return -1;
  return r->colkind[col] == BASE_SYMBOL ? TUPU_COL_SYMBOL : TUPU_COL_NUMBER;
}

int tupu_intern(TupuSession *s, const char *str, size_t len, uint32_t *out_id) {
  if (s == NULL || (str == NULL && len > 0) || out_id == NULL) return -1;
  return sym_intern(&s->prog.syms, str != NULL ? str : "", len, out_id, &s->err) == STATUS_OK ? 0 : -1;
}

const char *tupu_sym_str(const TupuSession *s, uint32_t id, size_t *out_len) {
  if (s == NULL || id >= s->prog.syms.n) return NULL;
  if (out_len != NULL) *out_len = s->prog.syms.len[id];
  return s->prog.syms.str[id];
}

int tupu_add_row(TupuSession *s, int rel_id, const int64_t *vals, int ncols) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  Table *t = &s->eng.tabs[rel_id];
  if (ncols != t->ncols || (ncols > 0 && vals == NULL)) return -1;
  I128 *row = s->eng.cells;
  for (int c = 0; c < ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) row[c] = (I128)(uint32_t)vals[c];
    else row[c] = (I128)vals[c];
  }
  bool added = false;
  return table_add(t, table_hash_row(t, row), row, ORG_INPUT, 0, 0, NULL, 0, &added, &s->err) == STATUS_OK ? 0 : -1;
}

int tupu_add_row_i128(TupuSession *s, int rel_id, const TupuI128 *vals, int ncols) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  Table *t = &s->eng.tabs[rel_id];
  if (ncols != t->ncols || (ncols > 0 && vals == NULL)) return -1;
  I128 *row = s->eng.cells;
  for (int c = 0; c < ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) row[c] = (I128)(uint32_t)vals[c].lo;
    else row[c] = i128_from_tupu(vals[c]);
  }
  bool added = false;
  return table_add(t, table_hash_row(t, row), row, ORG_INPUT, 0, 0, NULL, 0, &added, &s->err) == STATUS_OK ? 0 : -1;
}

int tupu_eval(TupuSession *s, int *err_code, char *err_buf, size_t err_cap) {
  if (err_code != NULL) *err_code = 0;
  if (err_buf != NULL && err_cap > 0) err_buf[0] = '\0';
  if (s == NULL) return -1;
  if (s->prog.syms.n != s->ranked_nsyms || (s->prog.syms.n > 0 && s->prog.syms.rank == NULL)) {
    if (sym_build_rank(&s->prog.syms, &s->err) != STATUS_OK) {
      report_err(&s->err, err_code, err_buf, err_cap);
      return -1;
    }
    s->ranked_nsyms = s->prog.syms.n;
  }
  for (size_t i = 0; i < s->prog.rels.n; i++) {
    if (table_enable_bitset(&s->eng.tabs[i], (uint32_t)s->prog.syms.n, &s->err) != STATUS_OK) {
      report_err(&s->err, err_code, err_buf, err_cap);
      return -1;
    }
  }
  if (engine_eval(&s->eng, &s->err) != STATUS_OK) {
    report_err(&s->err, err_code, err_buf, err_cap);
    return -1;
  }
  return 0;
}

int tupu_replay(TupuSession *s, int *err_code, char *err_buf, size_t err_cap) {
  if (err_code != NULL) *err_code = 0;
  if (err_buf != NULL && err_cap > 0) err_buf[0] = '\0';
  if (s == NULL) return -1;
  if (engine_replay(&s->eng, &s->err) != STATUS_OK) {
    report_err(&s->err, err_code, err_buf, err_cap);
    return -1;
  }
  return 0;
}

uint32_t tupu_rel_count(const TupuSession *s, int rel_id) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return 0;
  return s->eng.tabs[rel_id].count;
}

int tupu_rel_read_row(const TupuSession *s, int rel_id, uint32_t row, int64_t *out_vals, int ncols) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  const Table *t = &s->eng.tabs[rel_id];
  if (row >= t->count || ncols != t->ncols || (ncols > 0 && out_vals == NULL)) return -1;
  for (int c = 0; c < ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) out_vals[c] = (int64_t)((const uint32_t *)t->col[c])[row];
    else out_vals[c] = (int64_t)((const I128 *)t->col[c])[row];
  }
  return 0;
}

int tupu_rel_read_row_i128(const TupuSession *s, int rel_id, uint32_t row, TupuI128 *out_vals, int ncols) {
  if (s == NULL || rel_id < 0 || (size_t)rel_id >= s->prog.rels.n) return -1;
  const Table *t = &s->eng.tabs[rel_id];
  if (row >= t->count || ncols != t->ncols || (ncols > 0 && out_vals == NULL)) return -1;
  for (int c = 0; c < ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) out_vals[c] = tupu_i128_from_u64(((const uint32_t *)t->col[c])[row]);
    else out_vals[c] = tupu_from_i128(((const I128 *)t->col[c])[row]);
  }
  return 0;
}
