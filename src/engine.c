#include "engine.h"
#include "num.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TUPU_COUNTERS
#include <time.h>
#endif

static Status arith_err(Engine *x) {
  return err_set(x->err, CODE_DATALOG_ARITH, NULL, 0, "integer overflow or division by zero");
}

static Status ex_eval(Engine *x, const Expr *e, I128 *out) {
  switch (e->kind) {
  case EX_NUM: *out = e->num; return STATUS_OK;
  case EX_SYM: *out = (I128)e->sym; return STATUS_OK;
  case EX_VAR: *out = x->vars[e->var]; return STATUS_OK;
  case EX_WILD: *out = 0; return STATUS_OK;
  case EX_NEG: {
    I128 a;
    if (ex_eval(x, e->a, &a) != STATUS_OK) return STATUS_ERR;
    return num_neg(a, out) ? STATUS_OK : arith_err(x);
  }
  case EX_FN: {
    I128 a, b = 0;
    if (ex_eval(x, e->a, &a) != STATUS_OK) return STATUS_ERR;
    if (e->fn == FN_ORD) {
      *out = (I128)x->prog->syms.rank[(uint32_t)a];
      return STATUS_OK;
    }
    if (ex_eval(x, e->b, &b) != STATUS_OK) return STATUS_ERR;
    *out = e->fn == FN_MAX ? (a > b ? a : b) : (a < b ? a : b);
    return STATUS_OK;
  }
  case EX_BIN: {
    I128 a, b;
    if (ex_eval(x, e->a, &a) != STATUS_OK || ex_eval(x, e->b, &b) != STATUS_OK) return STATUS_ERR;
    bool ok;
    switch (e->op) {
#ifdef MUTANT_WRAPADD
    case OP_ADD: *out = (I128)((U128)a + (U128)b); ok = true; break;
#else
    case OP_ADD: ok = num_add(a, b, out); break;
#endif
    case OP_SUB: ok = num_sub(a, b, out); break;
    case OP_MUL: ok = num_mul(a, b, out); break;
    case OP_DIV: ok = num_div(a, b, out); break;
    default: ok = num_rem(a, b, out); break;
    }
    return ok ? STATUS_OK : arith_err(x);
  }
  }
  return STATUS_OK;
}

/* ---- the job buffer (S101 F1, F2, F3): rows of ONE rule, so one head relation and one row width. A row is its hash (u64), its cells at
   their column width (symbol u32, number I128) and, with -P, its body row ids. The set `hs`/`hu` finds a row already buffered by this
   job: slot = hash & (hcap - 1), one used byte per slot, load factor at most 1/2. ---- */
static size_t cell_off(const Table *t, int c) {
  size_t o = 0;
  for (int i = 0; i < c; i++) o += t->kind[i] == BASE_SYMBOL ? sizeof(uint32_t) : sizeof(I128);
  return o;
}

static void pack_row(const Table *t, uint8_t *dst, const I128 *vals) {
  for (int c = 0; c < t->ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) {
      uint32_t v = (uint32_t)vals[c];
      memcpy(dst, &v, sizeof v);
      dst += sizeof v;
    } else {
      memcpy(dst, &vals[c], sizeof(I128));
      dst += sizeof(I128);
    }
  }
}

static void unpack_row(const Table *t, const uint8_t *src, I128 *vals) {
  for (int c = 0; c < t->ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) {
      uint32_t v;
      memcpy(&v, src, sizeof v);
      vals[c] = (I128)v;
      src += sizeof v;
    } else {
      memcpy(&vals[c], src, sizeof(I128));
      src += sizeof(I128);
    }
  }
}

static bool jobset_has(const Engine *x, const Table *ht, uint64_t h, const I128 *vals) {
  const Buf *b = &x->buf;
  if (b->hcap == 0) return false;
  size_t i = h & (b->hcap - 1);
  I128 *o = x->scratch;
  while (b->hu[i]) {
    uint32_t k = b->hs[i];
    if (b->hash[k] == h) {
      unpack_row(ht, b->cells + (size_t)k * b->rowbytes, o);
      bool eq = true;
      for (int c = 0; c < ht->ncols && eq; c++) eq = o[c] == vals[c];
      if (eq) return true;
    }
    i = (i + 1) & (b->hcap - 1);
  }
  return false;
}

static Status jobset_insert(Engine *x, uint32_t entry) {
  Buf *b = &x->buf;
  if (b->hcap == 0 || (size_t)(entry + 1) * 2 > b->hcap) {
    size_t nc = b->hcap == 0 ? 1024 : b->hcap * 2;
    uint32_t *ns = malloc(nc * sizeof *ns);
    uint8_t *nu = calloc(nc, 1);
    if (ns == NULL || nu == NULL) {
      free(ns);
      free(nu);
      return err_oom(x->err);
    }
    for (uint32_t k = 0; k < entry; k++) { /* the stored hashes: no row is hashed again */
      size_t j = b->hash[k] & (nc - 1);
      while (nu[j]) j = (j + 1) & (nc - 1);
      nu[j] = 1;
      ns[j] = k;
    }
    free(b->hs);
    free(b->hu);
    b->hs = ns;
    b->hu = nu;
    b->hcap = nc;
  }
  size_t j = b->hash[entry] & (b->hcap - 1);
  while (b->hu[j]) j = (j + 1) & (b->hcap - 1);
  b->hu[j] = 1;
  b->hs[j] = entry;
  return STATUS_OK;
}

static Status buf_push(Engine *x, const Table *ht, uint64_t h, const I128 *vals, const uint32_t *body, int nb) {
  Buf *b = &x->buf;
  if (b->n == b->cap) {
    size_t nc = b->cap == 0 ? 1024 : b->cap * 2;
    uint64_t *nh = realloc(b->hash, nc * sizeof *nh);
    if (nh == NULL) return err_oom(x->err);
    b->hash = nh;
    uint8_t *ncl = realloc(b->cells, nc * b->rowbytes + 1);
    if (ncl == NULL) return err_oom(x->err);
    b->cells = ncl;
    if (nb > 0) {
      uint32_t *nbp = realloc(b->bodies, nc * (size_t)nb * sizeof *nbp);
      if (nbp == NULL) return err_oom(x->err);
      b->bodies = nbp;
    }
    b->cap = nc;
  }
  uint32_t k = (uint32_t)b->n;
  b->hash[k] = h;
  pack_row(ht, b->cells + (size_t)k * b->rowbytes, vals);
  if (nb > 0) memcpy(b->bodies + (size_t)k * (size_t)nb, body, (size_t)nb * sizeof *body);
#ifdef MUTANT_PROVBODY
  if (nb > 0) b->bodies[(size_t)k * (size_t)nb] += 1;
#endif
  b->n++;
  return jobset_insert(x, k);
}

static Status emit_head(Engine *x) {
  const Rule *r = x->cur->rule;
  I128 *hv = x->hv;
  for (int i = 0; i < r->head.nargs; i++)
    if (ex_eval(x, r->head.args[i], &hv[i]) != STATUS_OK) return STATUS_ERR;
  if (x->mode == MODE_REPLAY) {
    for (int i = 0; i < r->head.nargs; i++)
      if (hv[i] != x->target[i]) return STATUS_OK;
    if (x->prov)
      for (int i = 0; i < x->cur->natoms; i++)
        if (x->body[i] != x->target_body[i]) return STATUS_OK;
    x->found = true;
    return STATUS_OK;
  }
  x->derivations++;
  const Table *ht = &x->tabs[r->head.rel];
  uint64_t h = table_hash_row(ht, hv); /* the one hash of this derivation (F3): the table test, the job set and the merge use it */
  if (table_contains_h(ht, h, hv)) return STATUS_OK;
  if (jobset_has(x, ht, h, hv)) return STATUS_OK;
  x->rows_derived++;
  return buf_push(x, ht, h, hv, x->body, x->prov ? x->cur->natoms : 0);
}

static void range_of(const Engine *x, const Step *s, uint32_t *lo, uint32_t *hi) {
  const RelRange *r = &x->rr[s->rel];
  switch (s->range) {
  case RANGE_FULL: *lo = 0; *hi = r->cnt; break;
  case RANGE_PREV: *lo = 0; *hi = r->ds; break;
  default: *lo = r->ds; *hi = r->cnt; break;
  }
}

static bool match_row(Engine *x, const Step *s, const Table *t, uint32_t row) {
  for (int i = 0; i < s->nargs; i++) {
    const ArgSlot *a = &s->args[i];
    switch (a->k) {
    case AS_CONST:
      if (tcell(t, i, row) != a->val) return false;
      break;
    case AS_BOUND:
    case AS_EQ:
      if (tcell(t, i, row) != x->vars[a->var]) return false;
      break;
    case AS_FREE: x->vars[a->var] = tcell(t, i, row); break;
    case AS_WILD: break;
    }
  }
  return true;
}

static Status ex_run(Engine *x, const Plan *p, int k, PlanEnd end);

/* first position in the ascending ids[0..n) whose id is >= v */
static uint32_t lower_bound(const uint32_t *ids, uint32_t n, uint32_t v) {
  uint32_t a = 0, b = n;
  while (a < b) {
    uint32_t m = a + (b - a) / 2;
    uint32_t id;
    memcpy(&id, &ids[m], sizeof id);
    if (id < v) a = m + 1;
    else b = m;
  }
  return a;
}

/* the free columns of an indexed atom from the bucket cells of one row: binds AS_FREE variables, checks AS_EQ (S102) */
static bool load_free(Engine *x, const Step *s, const Index *ix, const Table *t, const uint8_t *cells) {
  for (uint32_t i = 0; i < ix->nf; i++) {
    uint32_t c = ix->fcols[i];
    I128 v;
    if (t->kind[c] == BASE_SYMBOL) {
      uint32_t u;
      memcpy(&u, cells, sizeof u);
      v = (I128)u;
      cells += sizeof u;
    } else {
      memcpy(&v, cells, sizeof v);
      cells += sizeof v;
    }
    const ArgSlot *a = &s->args[c];
    if (a->k == AS_FREE) x->vars[a->var] = v;
    else if (a->k == AS_EQ && x->vars[a->var] != v) return false;
  }
  return true;
}

static Status step_atom(Engine *x, const Plan *p, int k, PlanEnd end) {
  const Step *s = &p->steps[k];
  const Table *t = &x->tabs[s->rel];
  uint32_t lo, hi;
  range_of(x, s, &lo, &hi);
  if (k == x->chunk_step && p == &x->cur->plan) { /* S80: this job covers one chunk of the first atom's range */
    if (lo < x->chunk_lo) lo = x->chunk_lo;
    if (hi > x->chunk_hi) hi = x->chunk_hi;
  }
  if (lo >= hi) return STATUS_OK;
  if (s->idx == NULL) {
    for (uint32_t row = lo; row < hi; row++) {
      if (!match_row(x, s, t, row)) continue;
      if (s->atom_pos >= 0) x->body[s->atom_pos] = row;
      if (ex_run(x, p, k + 1, end) != STATUS_OK) return STATUS_ERR;
      if (x->found) return STATUS_OK;
    }
    return STATUS_OK;
  }
  x->probes++;
  const Index *ix = s->idx;
  const Bucket *bk;
  if (ix->direct_sym) {
    const ArgSlot *a0 = &s->args[ix->cols[0]];
    uint32_t sym = a0->k == AS_CONST ? (uint32_t)a0->val : (uint32_t)x->vars[a0->var];
    bk = index_find_sym(ix, sym);
  } else {
    I128 *key = x->scratch;
    for (int i = 0; i < s->nargs; i++) {
      const ArgSlot *a = &s->args[i];
      if (a->k == AS_CONST) key[i] = a->val;
      else if (a->k == AS_BOUND) key[i] = x->vars[a->var];
    }
    bk = index_find(ix, t, key, x->kbuf);
  }
  if (bk == NULL) return STATUS_OK;
  const uint32_t *ids = bucket_ids(bk);
  const uint8_t *cells = bucket_cells(bk);
  uint32_t a = lower_bound(ids, bk->n, lo), b = lower_bound(ids, bk->n, hi); /* the range is a contiguous span of the bucket */
  if (k + 1 == p->nsteps && end == END_HEAD && x->mode == MODE_DERIVE && !x->prov) {
    const Rule *r = x->cur->rule;
    const Table *ht = &x->tabs[r->head.rel];
    if (ht->fast2 && ix->nf == 1 && s->args[ix->fcols[0]].k == AS_FREE && t->kind[ix->fcols[0]] == BASE_SYMBOL &&
        (r->head.args[0]->kind == EX_VAR || r->head.args[0]->kind == EX_SYM) &&
        (r->head.args[1]->kind == EX_VAR || r->head.args[1]->kind == EX_SYM)) {
      int fvar = s->args[ix->fcols[0]].var;
      bool var0 = r->head.args[0]->kind == EX_VAR && r->head.args[0]->var == fvar;
      bool var1 = r->head.args[1]->kind == EX_VAR && r->head.args[1]->var == fvar;
      uint32_t c0 = var0 ? 0 : (r->head.args[0]->kind == EX_SYM ? r->head.args[0]->sym : (uint32_t)x->vars[r->head.args[0]->var]);
      uint32_t c1 = var1 ? 0 : (r->head.args[1]->kind == EX_SYM ? r->head.args[1]->sym : (uint32_t)x->vars[r->head.args[1]->var]);
      I128 *hv = x->hv;
      if (!var0) hv[0] = (I128)c0;
      if (!var1) hv[1] = (I128)c1;
      x->derivations += (uint64_t)(b - a);
      if (ht->bits != NULL) {
        if (!var0 && var1) {
          const uint64_t *brow = c0 < ht->bit_nsyms ? ht->bits[c0] : NULL;
          for (uint32_t i = b; i-- > a;) {
            uint32_t fv;
            memcpy(&fv, cells + (size_t)i * sizeof(uint32_t), sizeof fv);
            if (brow != NULL && ((brow[fv >> 6] >> (fv & 63)) & 1ULL) != 0) continue;
            uint64_t fkey = ((uint64_t)c0 << 32) | (uint64_t)fv;
            uint64_t h = mix64(fkey);
            hv[1] = (I128)fv;
            if (jobset_has(x, ht, h, hv)) continue;
            x->rows_derived++;
            if (buf_push(x, ht, h, hv, NULL, 0) != STATUS_OK) return STATUS_ERR;
          }
        } else {
          for (uint32_t i = b; i-- > a;) {
            uint32_t fv;
            memcpy(&fv, cells + (size_t)i * sizeof(uint32_t), sizeof fv);
            uint32_t v0 = var0 ? fv : c0;
            uint32_t v1 = var1 ? fv : c1;
            if (v0 < ht->bit_nsyms && ht->bits[v0] != NULL && ((ht->bits[v0][v1 >> 6] >> (v1 & 63)) & 1ULL) != 0) continue;
            uint64_t fkey = ((uint64_t)v0 << 32) | (uint64_t)v1;
            uint64_t h = mix64(fkey);
            if (var0) hv[0] = (I128)v0;
            if (var1) hv[1] = (I128)v1;
            if (jobset_has(x, ht, h, hv)) continue;
            x->rows_derived++;
            if (buf_push(x, ht, h, hv, NULL, 0) != STATUS_OK) return STATUS_ERR;
          }
        }
        return STATUS_OK;
      }
      for (uint32_t i = b; i-- > a;) {
        uint32_t fv;
        memcpy(&fv, cells + (size_t)i * sizeof(uint32_t), sizeof fv);
        uint32_t v0 = var0 ? fv : c0;
        uint32_t v1 = var1 ? fv : c1;
        uint64_t fkey = ((uint64_t)v0 << 32) | (uint64_t)v1;
        uint64_t h = mix64(fkey);
        if (table_contains_fast2(ht, fkey, h)) continue;
        if (var0) hv[0] = (I128)v0;
        if (var1) hv[1] = (I128)v1;
        if (jobset_has(x, ht, h, hv)) continue;
        x->rows_derived++;
        if (buf_push(x, ht, h, hv, NULL, 0) != STATUS_OK) return STATUS_ERR;
      }
      return STATUS_OK;
    }
  }
  for (uint32_t i = b; i-- > a;) { /* newest row first: the order of the chains this replaces, so the first derivation of a row (its provenance) is unchanged */
    if (!load_free(x, s, ix, t, cells + (size_t)i * ix->fbytes)) continue;
    if (s->atom_pos >= 0) memcpy(&x->body[s->atom_pos], &ids[i], sizeof(uint32_t));
    if (ex_run(x, p, k + 1, end) != STATUS_OK) return STATUS_ERR;
    if (x->found) return STATUS_OK;
  }
  return STATUS_OK;
}

static bool neg_exists(Engine *x, const Step *s) {
  const Table *t = &x->tabs[s->rel];
  if (s->exact) {
    I128 *vals = x->scratch;
    for (int i = 0; i < s->nargs; i++) vals[i] = s->args[i].k == AS_CONST ? s->args[i].val : x->vars[s->args[i].var];
    return table_contains(t, vals);
  }
  if (s->idx == NULL) return t->count > 0; /* only wildcards */
  x->probes++;
  const Index *ix = s->idx;
  const Bucket *bk;
  if (ix->direct_sym) {
    const ArgSlot *a0 = &s->args[ix->cols[0]];
    uint32_t sym = a0->k == AS_CONST ? (uint32_t)a0->val : (uint32_t)x->vars[a0->var];
    bk = index_find_sym(ix, sym);
  } else {
    I128 *key = x->scratch;
    for (int i = 0; i < s->nargs; i++) {
      const ArgSlot *a = &s->args[i];
      if (a->k == AS_CONST) key[i] = a->val;
      else if (a->k == AS_BOUND) key[i] = x->vars[a->var];
    }
    bk = index_find(ix, t, key, x->kbuf);
  }
  return bk != NULL && bk->n > 0; /* the key matched exactly: any row of the bucket is a witness */
}

static bool compare(CmpOp op, I128 a, I128 b) {
  switch (op) {
  case CMP_EQ: return a == b;
  case CMP_NE: return a != b;
  case CMP_LT: return a < b;
  case CMP_LE: return a <= b;
  case CMP_GT: return a > b;
  default: return a >= b;
  }
}

static Status step_agg(Engine *x, const Plan *p, int k, PlanEnd end) {
  const Step *s = &p->steps[k];
  Acc a = { s->agg, 0, false, s->l };
  Acc *saved = x->acc;
  x->acc = &a;
  Status st = ex_run(x, s->sub, 0, END_AGG);
  x->acc = saved;
  if (st != STATUS_OK) return STATUS_ERR;
  if (!a.any && (s->agg == AGG_MIN || s->agg == AGG_MAX)) return STATUS_OK; /* an empty set: the rule does not fire */
  if (s->target_bound) {
    if (x->vars[s->var] != a.acc) return STATUS_OK;
  } else {
    x->vars[s->var] = a.acc;
  }
  return ex_run(x, p, k + 1, end);
}

static Status accumulate(Engine *x) {
  Acc *a = x->acc;
  I128 v = 0;
  if (a->kind != AGG_COUNT && ex_eval(x, a->expr, &v) != STATUS_OK) return STATUS_ERR;
  switch (a->kind) {
  case AGG_COUNT:
    if (!num_add(a->acc, 1, &a->acc)) return arith_err(x);
    break;
  case AGG_SUM:
    if (!num_add(a->acc, v, &a->acc)) return arith_err(x);
    break;
  case AGG_MIN:
    if (!a->any || v < a->acc) a->acc = v;
    break;
  case AGG_MAX:
    if (!a->any || v > a->acc) a->acc = v;
    break;
  }
  a->any = true;
  return STATUS_OK;
}

static Status ex_run(Engine *x, const Plan *p, int k, PlanEnd end) {
  if (k == p->nsteps) return end == END_HEAD ? emit_head(x) : accumulate(x);
  const Step *s = &p->steps[k];
  switch (s->kind) {
  case ST_ATOM: return step_atom(x, p, k, end);
  case ST_NEG:
    if (neg_exists(x, s)) return STATUS_OK;
    return ex_run(x, p, k + 1, end);
  case ST_FILTER: {
    I128 a, b;
    if (ex_eval(x, s->l, &a) != STATUS_OK || ex_eval(x, s->r, &b) != STATUS_OK) return STATUS_ERR;
    if (!compare(s->cmp, a, b)) return STATUS_OK;
    return ex_run(x, p, k + 1, end);
  }
  case ST_BIND: {
    I128 v;
    if (ex_eval(x, s->l, &v) != STATUS_OK) return STATUS_ERR;
    x->vars[s->var] = v;
    return ex_run(x, p, k + 1, end);
  }
  case ST_AGG: return step_agg(x, p, k, end);
  }
  return STATUS_OK;
}

static void tpool_destroy(struct ThreadPool *tp);

/* ---- open: tables, plans, facts ---- */
void engine_close(Engine *x) {
  tpool_destroy(x->tpool);
  free(x->jl.p);
  if (x->tabs != NULL)
    for (size_t i = 0; i < x->prog->rels.n; i++) table_free(&x->tabs[i]);
  free(x->tabs);
  free(x->vars);
  free(x->scratch);
  free(x->kbuf);
  free(x->hv);
  free(x->cells);
  free(x->body);
  free(x->rr);
  free(x->naive_by_comp);
  free(x->naive_first);
  free(x->delta_by_comp);
  free(x->delta_first);
  free(x->comp_recursive);
  free(x->before);
#ifdef TUPU_COUNTERS
  free(x->rel_derived);
  free(x->comp_rounds);
  free(x->comp_ns);
  free(x->rule_probes);
  free(x->rule_derivations);
  free(x->rule_ns);
#endif
  strata_free(&x->strata);
  memset(x, 0, sizeof *x);
}

static Status load_program_facts(Engine *x, Err *e) {
  Program *p = x->prog;
  for (size_t i = 0; i < p->facts.n; i++) {
    const Fact *f = &p->facts.p[i];
    I128 *vals = x->cells;
    for (int c = 0; c < f->atom.nargs; c++) vals[c] = f->atom.args[c]->kind == EX_SYM ? (I128)f->atom.args[c]->sym : f->atom.args[c]->num;
    bool added;
    if (table_add(&x->tabs[f->atom.rel], table_hash_row(&x->tabs[f->atom.rel], vals), vals, ORG_FACT, (uint32_t)i, 0, NULL, 0, &added, e) != STATUS_OK)
      return STATUS_ERR;
  }
  return STATUS_OK;
}

static void group_variants_by_comp(const Variants *vs, int ncomps, size_t *first, size_t *by_comp) {
  for (size_t i = 0; i < vs->n; i++) first[vs->p[i].comp + 1]++;
  for (int comp = 0; comp < ncomps; comp++) first[comp + 1] += first[comp];
  for (size_t i = 0; i < vs->n; i++) by_comp[first[vs->p[i].comp]++] = i;
  for (int comp = ncomps; comp > 0; comp--) first[comp] = first[comp - 1];
  first[0] = 0;
}

static Status engine_open_common(Engine *x, Program *p, const char *facts_dir, uint64_t budget, bool prov, size_t nthreads, Err *e) {
  memset(x, 0, sizeof *x);
  x->nthreads = nthreads == 0 ? 1 : nthreads;
  x->chunk_step = -1;
  x->prog = p;
  x->budget = budget;
  x->prov = prov;
  x->err = e;
  if (stratify(p, &x->strata, e) != STATUS_OK) return STATUS_ERR;
  size_t n = p->rels.n;
  x->tabs = calloc(n + 1, sizeof *x->tabs);
  x->rr = calloc(n + 1, sizeof *x->rr);
  if (x->tabs == NULL || x->rr == NULL) return err_oom(e);
  size_t maxcols = 1;
  for (size_t i = 0; i < n; i++) {
    if ((size_t)p->rels.p[i].ncols > maxcols) maxcols = (size_t)p->rels.p[i].ncols;
    if (table_init(&x->tabs[i], &p->rels.p[i], prov, e) != STATUS_OK) return STATUS_ERR;
  }
  x->ncols_max = maxcols;
  x->scratch = malloc((maxcols + 1) * sizeof *x->scratch);
  x->hv = malloc((maxcols + 1) * sizeof *x->hv);
  x->kbuf = malloc((maxcols + 1) * sizeof(I128));
  x->cells = malloc((maxcols + 1) * sizeof *x->cells);
  if (x->scratch == NULL || x->hv == NULL || x->kbuf == NULL || x->cells == NULL) return err_oom(e);
  int maxvars = 1, maxatoms = 1;
  for (size_t i = 0; i < p->rules.n; i++) {
    if (p->rules.p[i].nvars > maxvars) maxvars = p->rules.p[i].nvars;
    if (p->rules.p[i].nlits > maxatoms) maxatoms = p->rules.p[i].nlits;
  }
  x->nvars_max = (size_t)maxvars;
  x->natoms_max = (size_t)maxatoms;
  x->vars = calloc((size_t)maxvars + 1, sizeof *x->vars);
  x->body = calloc((size_t)maxatoms + 1, sizeof *x->body);
  if (x->vars == NULL || x->body == NULL) return err_oom(e);
  /* input rows first, in declaration order; then the program facts in text order */
  if (facts_dir != NULL) {
    for (size_t i = 0; i < n; i++) {
      const Relation *r = &p->rels.p[i];
      if (!r->is_input) continue;
      size_t len = strlen(facts_dir) + r->name.n + 16;
      char *path = arena_alloc(&p->arena, len);
      if (path == NULL) return err_oom(e);
      (void)snprintf(path, len, "%s/%.*s.facts", facts_dir, (int)r->name.n, r->name.s);
      if (facts_load(&x->tabs[i], r, &p->syms, path, e) != STATUS_OK) return STATUS_ERR;
    }
  }
  if (load_program_facts(x, e) != STATUS_OK) return STATUS_ERR;
  if (sym_build_rank(&p->syms, e) != STATUS_OK) return STATUS_ERR;
  if (plan_program(p, &x->strata, x->tabs, &x->naive, &x->delta, e) != STATUS_OK) return STATUS_ERR;
  x->naive_first = calloc((size_t)x->strata.ncomps + 1, sizeof *x->naive_first);
  x->delta_first = calloc((size_t)x->strata.ncomps + 1, sizeof *x->delta_first);
  x->comp_recursive = calloc((size_t)x->strata.ncomps + 1, sizeof *x->comp_recursive);
  x->naive_by_comp = malloc((x->naive.n + 1) * sizeof *x->naive_by_comp);
  x->delta_by_comp = malloc((x->delta.n + 1) * sizeof *x->delta_by_comp);
  x->before = malloc((n + 1) * sizeof *x->before);
  if (x->naive_first == NULL || x->delta_first == NULL || x->comp_recursive == NULL || x->naive_by_comp == NULL ||
      x->delta_by_comp == NULL || x->before == NULL)
    return err_oom(e);
  group_variants_by_comp(&x->naive, x->strata.ncomps, x->naive_first, x->naive_by_comp);
  group_variants_by_comp(&x->delta, x->strata.ncomps, x->delta_first, x->delta_by_comp);
  for (size_t r = 0; r < n; r++)
    if (x->strata.recursive[r]) x->comp_recursive[x->strata.comp[r]] = true;
  for (size_t i = 0; i < n; i++)
    if (table_enable_bitset(&x->tabs[i], (uint32_t)p->syms.n, e) != STATUS_OK) return STATUS_ERR;
  return STATUS_OK;
}

Status engine_open(Engine *x, Program *p, const char *facts_dir, uint64_t budget, bool prov, size_t nthreads, Err *e) {
  return engine_open_common(x, p, facts_dir, budget, prov, nthreads, e);
}

Status engine_open_mem(Engine *x, Program *p, uint64_t budget, size_t nthreads, Err *e) {
  return engine_open_common(x, p, NULL, budget, false, nthreads, e);
}

Status engine_reset(Engine *x, Err *e) {
  x->err = e;
  Program *p = x->prog;
  for (size_t i = 0; i < p->rels.n; i++) table_clear(&x->tabs[i]);
  memset(x->rr, 0, (p->rels.n + 1) * sizeof *x->rr);
  x->added = 0;
  x->derivations = 0;
  x->rows_derived = 0;
  x->probes = 0;
  x->rounds = 0;
  x->round = 0;
  return load_program_facts(x, e);
}

/* ---- evaluation ---- */
static Status sync_indexes(Engine *x, int comp) {
  Program *p = x->prog;
  for (size_t r = 0; r < p->rels.n; r++) {
    Table *t = &x->tabs[r];
    if (t->count == 0 || x->strata.comp[r] > comp) continue;
    for (int i = 0; i < t->nidx; i++)
      if (index_sync(t->idx[i], t, x->err) != STATUS_OK) return STATUS_ERR;
  }
  return STATUS_OK;
}

static Status merge(Engine *x, JobList *jl, uint32_t round, uint64_t *new_rows);
/* ---- S80: a round is a list of jobs, one per (rule variant, chunk of 4096 rows of its first atom), in that order ---- */
#define CHUNK_ROWS 4096u

typedef struct {
  Engine *g;
  JobList *jobs;
  atomic_size_t *next;
  atomic_ullong derivations, rows, probes;
  atomic_size_t seq;
} Pool;

static int first_atom_step(const Variant *v) {
  for (int k = 0; k < v->plan.nsteps; k++)
    if (v->plan.steps[k].kind == ST_ATOM) return k;
  return -1;
}

static void job_free(Job *j) {
  free(j->buf.hash);
  free(j->buf.cells);
  free(j->buf.bodies);
}

static Status jobs_push(JobList *l, const Variant *v, int step, uint32_t lo, uint32_t hi, Err *e) {
  if (l->n == l->cap) {
    size_t nc = l->cap == 0 ? 64 : l->cap * 2;
    Job *np = realloc(l->p, nc * sizeof *np);
    if (np == NULL) return err_oom(e);
    l->p = np;
    l->cap = nc;
  }
  Job *j = &l->p[l->n++];
  memset(j, 0, sizeof *j);
  j->v = v;
  j->step = step;
  j->lo = lo;
  j->hi = hi;
  return STATUS_OK;
}

/* one worker: a private copy of the execution state; tables, indexes and ranges are shared and read-only in a round */
typedef struct {
  Engine w;
  bool ok;
} Worker;

static Status worker_init(Worker *wk, const Engine *g) {
  wk->w = *g;
  memset(&wk->w.buf, 0, sizeof wk->w.buf);
  size_t nv = g->nvars_max + 1, nc = g->ncols_max + 1;
  wk->w.vars = calloc(nv, sizeof *wk->w.vars);
  wk->w.body = calloc(g->natoms_max + 1, sizeof *wk->w.body);
  wk->w.scratch = malloc(nc * sizeof *wk->w.scratch);
  wk->w.hv = malloc(nc * sizeof *wk->w.hv);
  wk->w.kbuf = malloc(nc * sizeof(I128));
  wk->w.cells = NULL;
  wk->w.derivations = wk->w.rows_derived = wk->w.probes = 0;
  wk->ok = wk->w.vars && wk->w.body && wk->w.scratch && wk->w.hv && wk->w.kbuf;
  return wk->ok ? STATUS_OK : STATUS_ERR;
}

static void worker_free(Worker *wk) {
  free(wk->w.vars);
  free(wk->w.body);
  free(wk->w.scratch);
  free(wk->w.hv);
  free(wk->w.kbuf);
}

static void run_job(Engine *w, Job *j) {
  w->cur = j->v;
  w->chunk_step = j->step;
  w->chunk_lo = j->lo;
  w->chunk_hi = j->hi;
  w->err = &j->err;
  w->buf = j->buf;
  w->buf.rowbytes = cell_off(&w->tabs[j->v->rule->head.rel], w->tabs[j->v->rule->head.rel].ncols);
#ifdef TUPU_COUNTERS
  uint64_t p0 = w->probes, d0 = w->derivations;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
  j->st = ex_run(w, &j->v->plan, 0, END_HEAD);
#ifdef TUPU_COUNTERS
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  j->probes = w->probes - p0;
  j->derivations = w->derivations - d0;
  j->ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
#endif
  j->buf = w->buf;
  free(j->buf.hs); /* F1: the job set is private to the job; the merge dedups across jobs */
  free(j->buf.hu);
  j->buf.hs = NULL;
  j->buf.hu = NULL;
  j->buf.hcap = 0;
}

static void worker_run_pool(Worker *wk, Pool *pl) {
  wk->w.round = pl->g->round;
  wk->w.derivations = 0;
  wk->w.rows_derived = 0;
  wk->w.probes = 0;
  for (;;) {
    size_t i = atomic_fetch_add(pl->next, 1);
    if (i >= pl->jobs->n) break;
    run_job(&wk->w, &pl->jobs->p[i]);
    pl->jobs->p[i].seq = atomic_fetch_add(&pl->seq, 1);
  }
  atomic_fetch_add(&pl->derivations, wk->w.derivations);
  atomic_fetch_add(&pl->rows, wk->w.rows_derived);
  atomic_fetch_add(&pl->probes, wk->w.probes);
}

typedef struct {
  struct ThreadPool *tp;
  size_t id;
} WorkerArg;

struct ThreadPool {
  pthread_mutex_t mu;
  pthread_cond_t cv_go;
  pthread_cond_t cv_done;
  uint64_t epoch;
  size_t active;
  size_t nrun;
  bool stop;
  Pool *cur_pl;
  size_t nworkers;
  pthread_t th[63];
  WorkerArg args[63];
  Worker workers[63];
  Worker main_wk;
  bool sync_init;
};

static void *tpool_worker_main(void *arg) {
  WorkerArg *wa = arg;
  struct ThreadPool *tp = wa->tp;
  size_t id = wa->id;
  Worker *wk = &tp->workers[id];
  uint64_t my_epoch = 0;
  pthread_mutex_lock(&tp->mu);
  for (;;) {
    while (!tp->stop && (tp->epoch == my_epoch || id >= tp->nrun)) pthread_cond_wait(&tp->cv_go, &tp->mu);
    if (tp->stop) {
      pthread_mutex_unlock(&tp->mu);
      return NULL;
    }
    my_epoch = tp->epoch;
    Pool *pl = tp->cur_pl;
    pthread_mutex_unlock(&tp->mu);
    worker_run_pool(wk, pl);
    pthread_mutex_lock(&tp->mu);
    tp->active--;
    if (tp->active == 0) pthread_cond_signal(&tp->cv_done);
  }
}

static Status tpool_ensure(Engine *x, Err *e) {
  if (x->tpool != NULL) return STATUS_OK;
  struct ThreadPool *tp = calloc(1, sizeof *tp);
  if (tp == NULL) return err_oom(e);
  if (worker_init(&tp->main_wk, x) != STATUS_OK) {
    worker_free(&tp->main_wk);
    free(tp);
    return err_oom(e);
  }
  size_t nt_max = x->nthreads > 64 ? 64 : x->nthreads;
  size_t nw = nt_max > 1 ? nt_max - 1 : 0;
  if (nw > 0) {
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->cv_go, NULL);
    pthread_cond_init(&tp->cv_done, NULL);
    tp->sync_init = true;
    for (size_t i = 0; i < nw; i++) {
      if (worker_init(&tp->workers[i], x) != STATUS_OK) {
        worker_free(&tp->workers[i]);
        break;
      }
      tp->args[i].tp = tp;
      tp->args[i].id = i;
      if (pthread_create(&tp->th[i], NULL, tpool_worker_main, &tp->args[i]) != 0) {
        worker_free(&tp->workers[i]);
        break;
      }
      tp->nworkers++;
    }
  }
  x->tpool = tp;
  return STATUS_OK;
}

static void tpool_destroy(struct ThreadPool *tp) {
  if (tp == NULL) return;
  if (tp->nworkers > 0) {
    pthread_mutex_lock(&tp->mu);
    tp->stop = true;
    pthread_cond_broadcast(&tp->cv_go);
    pthread_mutex_unlock(&tp->mu);
    for (size_t i = 0; i < tp->nworkers; i++) {
      (void)pthread_join(tp->th[i], NULL);
      worker_free(&tp->workers[i]);
    }
  }
  if (tp->sync_init) {
    pthread_cond_destroy(&tp->cv_done);
    pthread_cond_destroy(&tp->cv_go);
    pthread_mutex_destroy(&tp->mu);
  }
  worker_free(&tp->main_wk);
  free(tp);
}

/* build the jobs of this round, run them on up to x->nthreads threads, then merge the buffers in job order */
static Status run_round(Engine *x, const Variants *vs, const size_t *by_comp, size_t first, size_t end, uint32_t round, uint64_t *new_rows) {
  JobList *jl = &x->jl;
  jl->n = 0;
  Status st = STATUS_OK;
  for (size_t idx = first; idx < end && st == STATUS_OK; idx++) {
    const Variant *v = &vs->p[by_comp[idx]];
    int fa = first_atom_step(v);
    if (fa < 0) {
      st = jobs_push(jl, v, -1, 0, 0, x->err);
      continue;
    }
    uint32_t lo, hi;
    range_of(x, &v->plan.steps[fa], &lo, &hi);
    uint32_t span = hi > lo ? hi - lo : 0;
    uint32_t chunk = span <= 4096u ? 64u : CHUNK_ROWS;
    for (uint32_t c = lo; c < hi && st == STATUS_OK; c += chunk) {
      uint32_t ch = hi - c > chunk ? c + chunk : hi;
      st = jobs_push(jl, v, fa, c, ch, x->err);
    }
  }
  if (st != STATUS_OK) {
    jl->n = 0;
    return st;
  }
  if (jl->n == 0) {
    *new_rows = 0;
    return STATUS_OK;
  }
  if (tpool_ensure(x, x->err) != STATUS_OK) {
    jl->n = 0;
    return STATUS_ERR;
  }
  struct ThreadPool *tp = x->tpool;
  atomic_size_t next = 0;
  Pool pl = { x, jl, &next, 0, 0, 0, 0 };
  size_t nrun = 0;
  if (tp->nworkers > 0 && jl->n > 1) nrun = jl->n - 1 < tp->nworkers ? jl->n - 1 : tp->nworkers;
  if (nrun > 0) {
    pthread_mutex_lock(&tp->mu);
    tp->cur_pl = &pl;
    tp->nrun = nrun;
    tp->active = nrun;
    tp->epoch++;
    pthread_cond_broadcast(&tp->cv_go);
    pthread_mutex_unlock(&tp->mu);
  }
  worker_run_pool(&tp->main_wk, &pl);
  if (nrun > 0) {
    pthread_mutex_lock(&tp->mu);
    while (tp->active > 0) pthread_cond_wait(&tp->cv_done, &tp->mu);
    pthread_mutex_unlock(&tp->mu);
  }
  x->derivations += atomic_load(&pl.derivations);
  x->rows_derived += atomic_load(&pl.rows);
  x->probes += atomic_load(&pl.probes);
#ifdef TUPU_COUNTERS
  for (size_t i = 0; i < jl->n; i++) {
    const Job *j = &jl->p[i];
    x->rel_derived[j->v->rule->head.rel] += j->buf.n;
    x->rule_probes[j->v->rule->index] += j->probes;
    x->rule_derivations[j->v->rule->index] += j->derivations;
    x->rule_ns[j->v->rule->index] += j->ns;
  }
#endif
  /* the first failed job in job order decides the refusal: the same for every thread count */
  for (size_t i = 0; i < jl->n && st == STATUS_OK; i++)
    if (jl->p[i].st != STATUS_OK) {
      *x->err = jl->p[i].err;
      st = STATUS_ERR;
    }
  /* a job nobody ran (every worker failed to allocate) leaves st unset: its status is STATUS_OK with an empty buffer, so
     check that every job was taken */
  if (st == STATUS_OK && atomic_load(&next) < jl->n) st = err_oom(x->err);
  if (st == STATUS_OK) st = merge(x, jl, round, new_rows);
  for (size_t i = 0; i < jl->n; i++) job_free(&jl->p[i]);
  jl->n = 0;
  return st;
}

#ifdef MUTANT_MERGEORDER
static int by_seq(const void *a, const void *b) {
  const Job *x = a, *y = b;
  return (x->seq > y->seq) - (x->seq < y->seq);
}
#endif
static Status merge(Engine *x, JobList *jl, uint32_t round, uint64_t *new_rows) {
  *new_rows = 0;
#ifdef MUTANT_MERGEORDER
  qsort(jl->p, jl->n, sizeof *jl->p, by_seq);
#endif
  for (size_t j = 0; j < jl->n; j++) {
    Buf *b = &jl->p[j].buf;
    const Rule *rule = jl->p[j].v->rule;
    Table *ht = &x->tabs[rule->head.rel];
    I128 *vals = x->cells;
    int nb = x->prov ? jl->p[j].v->natoms : 0;
    for (size_t i = 0; i < b->n; i++) {
      unpack_row(ht, b->cells + i * b->rowbytes, vals);
      bool added;
      if (table_add(ht, b->hash[i], vals, ORG_RULE, (uint32_t)rule->index, round, nb > 0 ? b->bodies + i * (size_t)nb : NULL, nb, &added, x->err) !=
          STATUS_OK)
        return STATUS_ERR;
      if (added) (*new_rows)++;
    }
  }
  x->added += *new_rows;
#ifndef MUTANT_NOBUDGET
  if (x->added > x->budget) return err_set(x->err, CODE_DATALOG_BUDGET, NULL, 0, "the row budget of %llu rows is exceeded", (unsigned long long)x->budget);
#endif
  return STATUS_OK;
}

Status engine_eval(Engine *x, Err *e) {
  x->err = e;
  Program *p = x->prog;
  size_t n = p->rels.n;
#ifdef TUPU_COUNTERS
  free(x->rel_derived);
  free(x->comp_rounds);
  free(x->comp_ns);
  free(x->rule_probes);
  free(x->rule_derivations);
  free(x->rule_ns);
  x->rule_probes = calloc(p->rules.n + 1, sizeof *x->rule_probes);
  x->rule_derivations = calloc(p->rules.n + 1, sizeof *x->rule_derivations);
  x->rule_ns = calloc(p->rules.n + 1, sizeof *x->rule_ns);
  if (x->rule_probes == NULL || x->rule_derivations == NULL || x->rule_ns == NULL) return err_oom(e);
  x->rel_derived = calloc(n + 1, sizeof *x->rel_derived);
  x->comp_rounds = calloc((size_t)x->strata.ncomps + 1, sizeof *x->comp_rounds);
  x->comp_ns = calloc((size_t)x->strata.ncomps + 1, sizeof *x->comp_ns);
  if (x->rel_derived == NULL || x->comp_rounds == NULL || x->comp_ns == NULL) return err_oom(e);
#endif
  for (int ci = 0; ci < x->strata.ncomps; ci++) {
#ifdef MUTANT_NEGEARLY
    int comp = x->strata.ncomps - 1 - ci;
#else
    int comp = ci;
#endif
    size_t nfirst = x->naive_first[comp], nend = x->naive_first[comp + 1];
    if (nfirst == nend) continue;
    bool any_work = false;
    for (size_t k = nfirst; k < nend; k++) {
      const Variant *v = &x->naive.p[x->naive_by_comp[k]];
      int fa = first_atom_step(v);
      if (fa < 0 || x->tabs[v->plan.steps[fa].rel].count > 0) {
        any_work = true;
        break;
      }
    }
    if (!any_work) {
      x->rounds++;
#ifdef TUPU_COUNTERS
      x->comp_rounds[comp]++;
#endif
      continue;
    }
    uint32_t round = 1;
#ifdef TUPU_COUNTERS
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    for (;;) {
      if (sync_indexes(x, comp) != STATUS_OK) return STATUS_ERR;
      /* ranges at the start of the round: delta = rows added by the previous round */
      for (size_t r = 0; r < n; r++) x->rr[r].cnt = x->tabs[r].count;
      x->round = round;
      for (size_t r = 0; r < n; r++) x->before[r] = x->tabs[r].count;
      uint64_t added = 0;
      Status st = run_round(x, round == 1 ? &x->naive : &x->delta,
                            round == 1 ? x->naive_by_comp : x->delta_by_comp,
                            round == 1 ? nfirst : x->delta_first[comp],
                            round == 1 ? nend : x->delta_first[comp + 1],
                            round, &added);
      if (st != STATUS_OK) return STATUS_ERR;
      x->rounds++;
#ifdef TUPU_COUNTERS
      x->comp_rounds[comp]++;
#endif
      for (size_t r = 0; r < n; r++) x->rr[r].ds = x->before[r];
      if (added == 0 || !x->comp_recursive[comp]) break;
      round++;
    }
#ifdef TUPU_COUNTERS
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    x->comp_ns[comp] += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
#endif
  }
  return STATUS_OK;
}

#ifdef TUPU_COUNTERS
static int name_lt(const Relation *a, const Relation *b) {
  size_t m = a->name.n < b->name.n ? a->name.n : b->name.n;
  int c = memcmp(a->name.s, b->name.s, m);
  return c != 0 ? c < 0 : a->name.n < b->name.n;
}

void engine_print_counters(const Engine *x) {
  const Program *p = x->prog;
  size_t n = p->rels.n;
  size_t *ord = malloc((n + 1) * sizeof *ord);
  if (ord == NULL || x->rel_derived == NULL) {
    free(ord);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    size_t j = i;
    while (j > 0 && name_lt(&p->rels.p[i], &p->rels.p[ord[j - 1]])) {
      ord[j] = ord[j - 1];
      j--;
    }
    ord[j] = i;
  }
  for (size_t k = 0; k < n; k++) {
    size_t r = ord[k];
    (void)fprintf(stderr, "rel %.*s rows=%llu derived=%llu\n", (int)p->rels.p[r].name.n, p->rels.p[r].name.s,
                  (unsigned long long)x->tabs[r].count, (unsigned long long)x->rel_derived[r]);
  }
  free(ord);
  for (int c = 0; c < x->strata.ncomps; c++)
    if (x->comp_rounds[c] > 0)
      (void)fprintf(stderr, "stratum %d rounds=%llu ns=%llu\n", c, (unsigned long long)x->comp_rounds[c], (unsigned long long)x->comp_ns[c]);
  for (size_t i = 0; i < p->rules.n; i++) {
    const Rule *r = &p->rules.p[i];
    (void)fprintf(stderr, "rule %d %s:%ld head=%.*s probes=%llu derivations=%llu ns=%llu\n", r->index, r->file != NULL ? r->file : "-", r->line,
                  (int)p->rels.p[r->head.rel].name.n, p->rels.p[r->head.rel].name.s, (unsigned long long)x->rule_probes[r->index],
                  (unsigned long long)x->rule_derivations[r->index], (unsigned long long)x->rule_ns[r->index]);
  }
}
#endif

/* ---- O6: provenance replay ---- */
static const Variant *naive_variant(const Engine *x, uint32_t rule) {
  for (size_t i = 0; i < x->naive.n; i++)
    if ((uint32_t)x->naive.p[i].rule->index == rule) return &x->naive.p[i];
  return NULL;
}

const Variant *engine_variant_of(const Engine *x, uint32_t rule) { return naive_variant(x, rule); }

Status engine_replay(Engine *x, Err *e) {
  x->err = e;
  Program *p = x->prog;
  if (sync_indexes(x, x->strata.ncomps) != STATUS_OK) return STATUS_ERR;
  for (size_t r = 0; r < p->rels.n; r++) {
    x->rr[r].cnt = x->tabs[r].count;
    x->rr[r].ds = 0;
  }
  for (size_t r = 0; r < p->rels.n; r++) {
    const Relation *rel = &p->rels.p[r];
    const Table *t = &x->tabs[r];
    if (!rel->is_output) continue;
    for (uint32_t row = 0; row < t->count; row++) {
      if (t->org_kind[row] != ORG_RULE) continue;
      const Variant *v = naive_variant(x, t->org_ref[row]);
      I128 *cells = x->cells;
      for (int c = 0; c < t->ncols; c++) cells[c] = tcell(t, c, row);
      x->mode = MODE_REPLAY;
      x->cur = v;
      x->target = cells;
      x->target_body = t->pool + t->poff[row];
      x->found = false;
      Status st = v == NULL ? STATUS_ERR : ex_run(x, &v->plan, 0, END_HEAD);
      x->mode = MODE_DERIVE;
      if (st != STATUS_OK && v != NULL) return STATUS_ERR;
      if (!x->found)
        return err_set(e, CODE_PROVENANCE_MISMATCH, NULL, 0, "row %u of %.*s does not derive again from rule %u", row,
                       (int)rel->name.n, rel->name.s, t->org_ref[row]);
    }
  }
  return STATUS_OK;
}
