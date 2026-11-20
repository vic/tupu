#include "engine.h"
#include "num.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* F1: is this row already buffered by the running job? A job has one rule, so one head relation: the rows compare cell by cell. */
static bool jobset_has(const Engine *x, const Table *ht, const I128 *vals, int nv, size_t *slot_out) {
  const Buf *b = &x->buf;
  if (b->hcap == 0) {
    *slot_out = 0;
    return false;
  }
  size_t i = table_hash_row(ht, vals) & (b->hcap - 1);
  while (b->hu[i]) {
    const I128 *o = b->vals + b->e[b->hs[i]].voff;
    bool eq = true;
    for (int c = 0; c < nv && eq; c++) eq = o[c] == vals[c];
    if (eq) return true;
    i = (i + 1) & (b->hcap - 1);
  }
  *slot_out = i;
  return false;
}

static Status jobset_add(Engine *x, const Table *ht, uint32_t entry, int nv) {
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
    for (uint32_t k = 0; k < entry; k++) {
      size_t j = table_hash_row(ht, b->vals + b->e[k].voff) & (nc - 1);
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
  size_t j = table_hash_row(ht, b->vals + b->e[entry].voff) & (b->hcap - 1);
  while (b->hu[j]) j = (j + 1) & (b->hcap - 1);
  b->hu[j] = 1;
  b->hs[j] = entry;
  (void)nv;
  return STATUS_OK;
}

static Status buf_push(Engine *x, uint32_t rel, uint32_t rule, const I128 *vals, int nv, const uint32_t *body, int nb) {
  Buf *b = &x->buf;
  if (b->n == b->cap) {
    size_t nc = b->cap == 0 ? 1024 : b->cap * 2;
    Entry *ne = realloc(b->e, nc * sizeof *ne);
    if (ne == NULL) return err_oom(x->err);
    b->e = ne;
    b->cap = nc;
  }
  if (b->nvals + (size_t)nv > b->vcap) {
    size_t nc = b->vcap == 0 ? 4096 : b->vcap * 2;
    while (nc < b->nvals + (size_t)nv) nc *= 2;
    I128 *nvp = realloc(b->vals, nc * sizeof *nvp);
    if (nvp == NULL) return err_oom(x->err);
    b->vals = nvp;
    b->vcap = nc;
  }
  if (b->nbodies + (size_t)nb > b->bcap) {
    size_t nc = b->bcap == 0 ? 4096 : b->bcap * 2;
    while (nc < b->nbodies + (size_t)nb) nc *= 2;
    uint32_t *nbp = realloc(b->bodies, nc * sizeof *nbp);
    if (nbp == NULL) return err_oom(x->err);
    b->bodies = nbp;
    b->bcap = nc;
  }
  Entry *e = &b->e[b->n++];
  e->rel = rel;
  e->rule = rule;
  e->voff = b->nvals;
  e->boff = b->nbodies;
  memcpy(b->vals + b->nvals, vals, (size_t)nv * sizeof *vals);
  b->nvals += (size_t)nv;
  if (nb > 0) memcpy(b->bodies + b->nbodies, body, (size_t)nb * sizeof *body);
#ifdef MUTANT_PROVBODY
  if (nb > 0) b->bodies[b->nbodies] += 1;
#endif
  b->nbodies += (size_t)nb;
  return STATUS_OK;
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
  if (table_contains(ht, hv)) return STATUS_OK;
  size_t dummy;
  if (jobset_has(x, ht, hv, r->head.nargs, &dummy)) return STATUS_OK;
  x->rows_derived++;
  uint32_t entry = (uint32_t)x->buf.n;
  if (buf_push(x, (uint32_t)r->head.rel, (uint32_t)r->index, hv, r->head.nargs, x->body, x->prov ? x->cur->natoms : 0) != STATUS_OK) return STATUS_ERR;
  return jobset_add(x, ht, entry, r->head.nargs);
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
  I128 *key = x->scratch;
  for (int i = 0; i < s->nargs; i++) {
    const ArgSlot *a = &s->args[i];
    if (a->k == AS_CONST) key[i] = a->val;
    else if (a->k == AS_BOUND) key[i] = x->vars[a->var];
  }
  uint32_t row;
  x->probes++;
  if (!index_find(s->idx, hash_cells(t, s->cols, s->nbound, key), &row)) return STATUS_OK;
  for (;;) {
    if (row < hi && row >= lo && match_row(x, s, t, row)) {
      if (s->atom_pos >= 0) x->body[s->atom_pos] = row;
      if (ex_run(x, p, k + 1, end) != STATUS_OK) return STATUS_ERR;
      if (x->found) return STATUS_OK;
    }
    uint32_t nr = s->idx->next[row];
    if (nr == row || nr < lo) break; /* chains run from newer to older rows */
    row = nr;
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
  I128 *key = x->scratch;
  for (int i = 0; i < s->nargs; i++) {
    const ArgSlot *a = &s->args[i];
    if (a->k == AS_CONST) key[i] = a->val;
    else if (a->k == AS_BOUND) key[i] = x->vars[a->var];
  }
  uint32_t row;
  x->probes++;
  if (!index_find(s->idx, hash_cells(t, s->cols, s->nbound, key), &row)) return false;
  for (;;) {
    bool ok = true;
    for (int i = 0; i < s->nargs && ok; i++) {
      const ArgSlot *a = &s->args[i];
      if (a->k == AS_CONST && tcell(t, i, row) != a->val) ok = false;
      else if (a->k == AS_BOUND && tcell(t, i, row) != x->vars[a->var]) ok = false;
    }
    if (ok) return true;
    uint32_t nr = s->idx->next[row];
    if (nr == row) return false;
    row = nr;
  }
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

/* ---- open: tables, plans, facts ---- */
void engine_close(Engine *x) {
  if (x->tabs != NULL)
    for (size_t i = 0; i < x->prog->rels.n; i++) table_free(&x->tabs[i]);
  free(x->tabs);
  free(x->vars);
  free(x->scratch);
  free(x->hv);
  free(x->cells);
  free(x->body);
  free(x->rr);
  free(x->buf.e);
  free(x->buf.vals);
  free(x->buf.bodies);
  strata_free(&x->strata);
  memset(x, 0, sizeof *x);
}

Status engine_open(Engine *x, Program *p, const char *facts_dir, uint64_t budget, bool prov, size_t nthreads, Err *e) {
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
  x->cells = malloc((maxcols + 1) * sizeof *x->cells);
  if (x->scratch == NULL || x->hv == NULL || x->cells == NULL) return err_oom(e);
  if (plan_program(p, &x->strata, x->tabs, &x->naive, &x->delta, e) != STATUS_OK) return STATUS_ERR;
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
  for (size_t i = 0; i < n; i++) {
    const Relation *r = &p->rels.p[i];
    if (!r->is_input) continue;
    size_t len = strlen(facts_dir) + r->name.n + 16;
    char *path = arena_alloc(&p->arena, len);
    if (path == NULL) return err_oom(e);
    (void)snprintf(path, len, "%s/%.*s.facts", facts_dir, (int)r->name.n, r->name.s);
    if (facts_load(&x->tabs[i], r, &p->syms, path, e) != STATUS_OK) return STATUS_ERR;
  }
  for (size_t i = 0; i < p->facts.n; i++) {
    const Fact *f = &p->facts.p[i];
    I128 *vals = x->cells;
    for (int c = 0; c < f->atom.nargs; c++) vals[c] = f->atom.args[c]->kind == EX_SYM ? (I128)f->atom.args[c]->sym : f->atom.args[c]->num;
    bool added;
    if (table_add(&x->tabs[f->atom.rel], vals, ORG_FACT, (uint32_t)i, 0, NULL, 0, &added, e) != STATUS_OK) return STATUS_ERR;
  }
  return sym_build_rank(&p->syms, e);
}

/* ---- evaluation ---- */
static Status sync_indexes(Engine *x, int comp) {
  Program *p = x->prog;
  for (size_t r = 0; r < p->rels.n; r++) {
    if (x->strata.comp[r] > comp) continue;
    Table *t = &x->tabs[r];
    for (int i = 0; i < t->nidx; i++)
      if (index_sync(t->idx[i], t, x->err) != STATUS_OK) return STATUS_ERR;
  }
  return STATUS_OK;
}

typedef struct JobList JobList;
static Status merge(Engine *x, JobList *jl, uint32_t round, uint64_t *new_rows);
/* ---- S80: a round is a list of jobs, one per (rule variant, chunk of 4096 rows of its first atom), in that order ---- */
#define CHUNK_ROWS 4096u

typedef struct {
  const Variant *v;
  int step; /* the first atom step of the plan, or -1 */
  uint32_t lo, hi;
  Buf buf;
  Err err;
  Status st;
  size_t seq; /* order of completion, used only by the MERGEORDER mutant */
} Job;

struct JobList {
  Job *p;
  size_t n, cap;
};

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
  free(j->buf.e);
  free(j->buf.vals);
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
  wk->w.cells = NULL;
  wk->w.derivations = wk->w.rows_derived = wk->w.probes = 0;
  wk->ok = wk->w.vars && wk->w.body && wk->w.scratch && wk->w.hv;
  return wk->ok ? STATUS_OK : STATUS_ERR;
}

static void worker_free(Worker *wk) {
  free(wk->w.vars);
  free(wk->w.body);
  free(wk->w.scratch);
  free(wk->w.hv);
}

static void run_job(Engine *w, Job *j) {
  w->cur = j->v;
  w->chunk_step = j->step;
  w->chunk_lo = j->lo;
  w->chunk_hi = j->hi;
  w->err = &j->err;
  w->buf = j->buf;
  j->st = ex_run(w, &j->v->plan, 0, END_HEAD);
  j->buf = w->buf;
  free(j->buf.hs); /* F1: the job set is private to the job; the merge dedups across jobs */
  free(j->buf.hu);
  j->buf.hs = NULL;
  j->buf.hu = NULL;
  j->buf.hcap = 0;
}

static void *pool_main(void *arg) {
  Pool *pl = arg;
  Worker wk;
  if (worker_init(&wk, pl->g) != STATUS_OK) {
    worker_free(&wk);
    /* an allocation failure of a worker: every job it would take is taken by the others; if none runs, the caller sees it */
    return NULL;
  }
  for (;;) {
    size_t i = atomic_fetch_add(pl->next, 1);
    if (i >= pl->jobs->n) break;
    run_job(&wk.w, &pl->jobs->p[i]);
    pl->jobs->p[i].seq = atomic_fetch_add(&pl->seq, 1);
  }
  atomic_fetch_add(&pl->derivations, wk.w.derivations);
  atomic_fetch_add(&pl->rows, wk.w.rows_derived);
  atomic_fetch_add(&pl->probes, wk.w.probes);
  worker_free(&wk);
  return NULL;
}

/* build the jobs of this round, run them on up to x->nthreads threads, then merge the buffers in job order */
static Status run_round(Engine *x, const Variants *vs, int comp, uint32_t round, uint64_t *new_rows) {
  JobList jl = { NULL, 0, 0 };
  Status st = STATUS_OK;
  for (size_t i = 0; i < vs->n && st == STATUS_OK; i++) {
    const Variant *v = &vs->p[i];
    if (v->comp != comp) continue;
    int fa = first_atom_step(v);
    if (fa < 0) {
      st = jobs_push(&jl, v, -1, 0, 0, x->err);
      continue;
    }
    uint32_t lo, hi;
    range_of(x, &v->plan.steps[fa], &lo, &hi);
    for (uint32_t c = lo; c < hi && st == STATUS_OK; c += CHUNK_ROWS) {
      uint32_t ch = hi - c > CHUNK_ROWS ? c + CHUNK_ROWS : hi;
      st = jobs_push(&jl, v, fa, c, ch, x->err);
    }
  }
  if (st != STATUS_OK) {
    free(jl.p);
    return st;
  }
  atomic_size_t next = 0;
  Pool pl = { x, &jl, &next, 0, 0, 0, 0 };
  size_t nt = x->nthreads < jl.n ? x->nthreads : jl.n;
  pthread_t th[64];
  size_t started = 0;
  if (nt > 64) nt = 64;
  for (size_t t = 1; t < nt; t++)
    if (pthread_create(&th[started], NULL, pool_main, &pl) == 0) started++;
  pool_main(&pl); /* the calling thread works too; with no thread started it runs every job */
  for (size_t t = 0; t < started; t++) (void)pthread_join(th[t], NULL);
  x->derivations += atomic_load(&pl.derivations);
  x->rows_derived += atomic_load(&pl.rows);
  x->probes += atomic_load(&pl.probes);
  /* the first failed job in job order decides the refusal: the same for every thread count */
  for (size_t i = 0; i < jl.n && st == STATUS_OK; i++)
    if (jl.p[i].st != STATUS_OK) {
      *x->err = jl.p[i].err;
      st = STATUS_ERR;
    }
  /* a job nobody ran (every worker failed to allocate) leaves st unset: its status is STATUS_OK with an empty buffer, so
     check that every job was taken */
  if (st == STATUS_OK && atomic_load(&next) < jl.n) st = err_oom(x->err);
  if (st == STATUS_OK) st = merge(x, &jl, round, new_rows);
  for (size_t i = 0; i < jl.n; i++) job_free(&jl.p[i]);
  free(jl.p);
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
    for (size_t i = 0; i < b->n; i++) {
      const Entry *en = &b->e[i];
      int nb = 0;
      if (x->prov) {
        const Rule *rule = &x->prog->rules.p[en->rule];
        for (int k = 0; k < rule->nlits; k++)
          if (rule->lits[k]->kind == LIT_POS) nb++;
      }
      bool added;
      if (table_add(&x->tabs[en->rel], b->vals + en->voff, ORG_RULE, en->rule, round, b->bodies + en->boff, nb, &added, x->err) !=
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
  for (int ci = 0; ci < x->strata.ncomps; ci++) {
#ifdef MUTANT_NEGEARLY
    int comp = x->strata.ncomps - 1 - ci;
#else
    int comp = ci;
#endif
    bool has_rules = false;
    for (size_t i = 0; i < x->naive.n; i++)
      if (x->naive.p[i].comp == comp) has_rules = true;
    if (!has_rules) continue;
    uint32_t round = 1;
    for (;;) {
      if (sync_indexes(x, comp) != STATUS_OK) return STATUS_ERR;
      /* ranges at the start of the round: delta = rows added by the previous round */
      for (size_t r = 0; r < n; r++) x->rr[r].cnt = x->tabs[r].count;
      x->round = round;
      uint32_t *before = malloc((n + 1) * sizeof *before);
      if (before == NULL) return err_oom(e);
      for (size_t r = 0; r < n; r++) before[r] = x->tabs[r].count;
      uint64_t added = 0;
      Status st = run_round(x, round == 1 ? &x->naive : &x->delta, comp, round, &added);
      if (st != STATUS_OK) {
        free(before);
        return STATUS_ERR;
      }
      x->rounds++;
      for (size_t r = 0; r < n; r++) x->rr[r].ds = before[r];
      free(before);
      bool recursive = false;
      for (size_t r = 0; r < n; r++)
        if (x->strata.comp[r] == comp && x->strata.recursive[r]) recursive = true;
      if (added == 0 || !recursive) break;
      round++;
    }
  }
  return STATUS_OK;
}

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
