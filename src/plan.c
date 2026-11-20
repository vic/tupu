#include "plan.h"
#include <string.h>

typedef struct {
  Program *p;
  const Strata *s;
  Table *tabs;
  const Rule *rule;
  int comp;
  Err *e;
} PCtx;

static bool expr_has_var(const Expr *x, int v) {
  if (x == NULL) return false;
  if (x->kind == EX_VAR) return x->var == v;
  return expr_has_var(x->a, v) || expr_has_var(x->b, v);
}
static bool atom_has_var(const Atom *a, int v) {
  for (int i = 0; i < a->nargs; i++)
    if (expr_has_var(a->args[i], v)) return true;
  return false;
}
static bool lit_has_var(const Lit *l, int v) {
  switch (l->kind) {
  case LIT_POS:
  case LIT_NEG: return atom_has_var(&l->atom, v);
  case LIT_CMP: return expr_has_var(l->l, v) || expr_has_var(l->r, v);
  case LIT_AGG:
    if (expr_has_var(l->l, v) || expr_has_var(l->aggexpr, v)) return true;
    for (int i = 0; i < l->nbody; i++)
      if (lit_has_var(l->body[i], v)) return true;
    return false;
  }
  return false;
}

/* every variable of x is bound */
static bool expr_bound(const Expr *x, const bool *bound) {
  if (x == NULL) return true;
  if (x->kind == EX_VAR) return bound[x->var];
  return expr_bound(x->a, bound) && expr_bound(x->b, bound);
}

static bool lone_unbound(const Expr *x, const bool *bound) { return x->kind == EX_VAR && !bound[x->var]; }

static void collect_vars(const Expr *x, int *out, int *n) {
  if (x == NULL) return;
  if (x->kind == EX_VAR) out[(*n)++] = x->var;
  collect_vars(x->a, out, n);
  collect_vars(x->b, out, n);
}

static Status plan_lits(PCtx *c, Lit *const *lits, int n, bool *bound, int delta_pos, bool top, Plan *out);

static int score_atom(const Atom *a, const bool *bound) {
  int s = 0;
  for (int i = 0; i < a->nargs; i++) {
    const Expr *x = a->args[i];
    if (x->kind == EX_NUM || x->kind == EX_SYM || (x->kind == EX_VAR && bound[x->var])) s++;
  }
  return s;
}

static Status build_atom_step(PCtx *c, const Lit *l, bool *bound, StepKind kind, int pos, int delta_pos, bool top, Step *st) {
  const Atom *a = &l->atom;
  memset(st, 0, sizeof *st);
  st->kind = kind;
  st->rel = a->rel;
  st->nargs = a->nargs;
  st->atom_pos = kind == ST_ATOM && top ? pos : -1;
  st->line = l->line;
  st->args = arena_alloc(&c->p->arena, (size_t)(a->nargs + 1) * sizeof *st->args);
  if (st->args == NULL) return err_oom(c->e);
  bool *seen = arena_alloc(&c->p->arena, (size_t)(c->rule->nvars + 1));
  if (seen == NULL) return err_oom(c->e);
  memset(seen, 0, (size_t)(c->rule->nvars + 1));
  uint32_t mask = 0;
  bool wild = false;
  for (int i = 0; i < a->nargs; i++) {
    const Expr *x = a->args[i];
    ArgSlot *s = &st->args[i];
    memset(s, 0, sizeof *s);
    if (x->kind == EX_NUM) { s->k = AS_CONST; s->val = x->num; mask |= (uint32_t)1 << i; }
    else if (x->kind == EX_SYM) { s->k = AS_CONST; s->val = (I128)x->sym; mask |= (uint32_t)1 << i; }
    else if (x->kind == EX_WILD) { s->k = AS_WILD; wild = true; }
    else {
      s->var = x->var;
      if (bound[x->var]) { s->k = AS_BOUND; mask |= (uint32_t)1 << i; }
      else if (seen[x->var]) s->k = AS_EQ;
      else { s->k = AS_FREE; seen[x->var] = true; }
    }
  }
  st->mask = mask;
  st->exact = !wild && kind == ST_NEG;
  if (kind == ST_ATOM) {
    for (int i = 0; i < a->nargs; i++)
      if (st->args[i].k == AS_FREE) bound[st->args[i].var] = true;
    if (!top || delta_pos < 0) st->range = RANGE_FULL;
    else if (pos == delta_pos) st->range = RANGE_DELTA;
    else if (c->s->comp[a->rel] == c->comp) st->range = pos < delta_pos ? RANGE_FULL : RANGE_PREV;
    else st->range = RANGE_FULL;
    if (st->range != RANGE_DELTA && mask != 0) {
      if (table_index(&c->tabs[a->rel], mask, &st->idx, c->e) != STATUS_OK) return STATUS_ERR;
    }
  } else if (mask != 0 && !st->exact) {
    if (table_index(&c->tabs[a->rel], mask, &st->idx, c->e) != STATUS_OK) return STATUS_ERR;
  }
  return STATUS_OK;
}

static Status plan_lits(PCtx *c, Lit *const *lits, int n, bool *bound, int delta_pos, bool top, Plan *out) {
  Program *p = c->p;
  bool *placed = arena_alloc(&p->arena, (size_t)(n + 1));
  Step *steps = arena_alloc(&p->arena, (size_t)(n + 1) * sizeof *steps);
  int *tpos = arena_alloc(&p->arena, (size_t)(n + 1) * sizeof *tpos); /* text position among positive atoms */
  if (placed == NULL || steps == NULL || tpos == NULL) return err_oom(c->e);
  memset(placed, 0, (size_t)n);
  int npos = 0, ns = 0;
  for (int i = 0; i < n; i++) tpos[i] = lits[i]->kind == LIT_POS ? npos++ : -1;
  int remaining_pos = npos;
  bool delta_done = delta_pos < 0;
  for (;;) {
    /* place every literal that has become ready, in text order, until none is */
    bool progress = true;
    while (progress) {
      progress = false;
      for (int i = 0; i < n; i++) {
        if (placed[i] || lits[i]->kind == LIT_POS) continue;
        const Lit *l = lits[i];
        Step *st = &steps[ns];
        if (l->kind == LIT_NEG) {
          bool ok = true;
          for (int k = 0; k < l->atom.nargs; k++) {
            const Expr *x = l->atom.args[k];
            if (x->kind == EX_VAR && !bound[x->var]) ok = false;
          }
          if (!ok) continue;
          if (build_atom_step(c, l, bound, ST_NEG, -1, delta_pos, top, st) != STATUS_OK) return STATUS_ERR;
        } else if (l->kind == LIT_CMP) {
          memset(st, 0, sizeof *st);
          st->line = l->line;
          st->cmp = l->cmp;
          if (l->cmp == CMP_EQ && lone_unbound(l->l, bound) && expr_bound(l->r, bound)) {
            st->kind = ST_BIND;
            st->var = l->l->var;
            st->l = l->r;
            bound[st->var] = true;
          } else if (l->cmp == CMP_EQ && lone_unbound(l->r, bound) && expr_bound(l->l, bound)) {
            st->kind = ST_BIND;
            st->var = l->r->var;
            st->l = l->l;
            bound[st->var] = true;
          } else if (expr_bound(l->l, bound) && expr_bound(l->r, bound)) {
            st->kind = ST_FILTER;
            st->l = l->l;
            st->r = l->r;
          } else continue;
        } else { /* LIT_AGG */
          /* shared variables occur outside the aggregate and must be bound before it runs */
          int vars[64], nv = 0;
          collect_vars(l->aggexpr, vars, &nv);
          for (int b = 0; b < l->nbody && nv < 56; b++) {
            const Lit *bl = l->body[b];
            if (bl->kind == LIT_POS || bl->kind == LIT_NEG)
              for (int k = 0; k < bl->atom.nargs; k++) collect_vars(bl->atom.args[k], vars, &nv);
            else if (bl->kind == LIT_CMP) { collect_vars(bl->l, vars, &nv); collect_vars(bl->r, vars, &nv); }
          }
          bool ready = true;
          for (int k = 0; k < nv && ready; k++) {
            int v = vars[k];
            if (bound[v] || v == l->l->var) continue;
            bool outside = atom_has_var(&c->rule->head, v);
            for (int j = 0; j < c->rule->nlits && !outside; j++)
              if (c->rule->lits[j] != l && lit_has_var(c->rule->lits[j], v)) outside = true;
            if (outside) ready = false;
          }
          if (!ready) continue;
          memset(st, 0, sizeof *st);
          st->kind = ST_AGG;
          st->line = l->line;
          st->agg = l->agg;
          st->l = l->aggexpr;
          st->var = l->l->var;
          st->target_bound = bound[st->var];
          bool *b2 = arena_alloc(&p->arena, (size_t)(c->rule->nvars + 1));
          Plan *sub = arena_alloc(&p->arena, sizeof *sub);
          if (b2 == NULL || sub == NULL) return err_oom(c->e);
          memcpy(b2, bound, (size_t)c->rule->nvars);
          if (plan_lits(c, l->body, l->nbody, b2, -1, false, sub) != STATUS_OK) return STATUS_ERR;
          st->sub = sub;
          bound[st->var] = true;
        }
        if (st->kind != ST_AGG && st->kind != ST_NEG) { /* FILTER, BIND already filled */ }
        st->line = l->line;
        placed[i] = true;
        ns++;
        progress = true;
      }
    }
    if (remaining_pos == 0) break;
    int best = -1, best_score = -1;
    if (!delta_done) {
      for (int i = 0; i < n; i++)
        if (!placed[i] && tpos[i] == delta_pos) best = i;
      delta_done = true;
    } else {
      for (int i = 0; i < n; i++) {
        if (placed[i] || lits[i]->kind != LIT_POS) continue;
        int sc = score_atom(&lits[i]->atom, bound);
        if (sc > best_score) {
          best_score = sc;
          best = i;
        }
      }
    }
    if (best < 0) break;
    if (build_atom_step(c, lits[best], bound, ST_ATOM, tpos[best], delta_pos, top, &steps[ns]) != STATUS_OK) return STATUS_ERR;
    placed[best] = true;
    ns++;
    remaining_pos--;
  }
  for (int i = 0; i < n; i++)
    if (!placed[i])
      return err_set(c->e, CODE_RULE_UNSAFE, c->rule->file, lits[i]->line, "a literal uses a variable that no positive atom binds");
  out->steps = steps;
  out->nsteps = ns;
  return STATUS_OK;
}

static Status plan_variant(PCtx *c, const Rule *r, int delta_pos, Variant *v) {
  c->rule = r;
  bool *bound = arena_alloc(&c->p->arena, (size_t)(r->nvars + 1));
  if (bound == NULL) return err_oom(c->e);
  memset(bound, 0, (size_t)(r->nvars + 1));
  v->rule = r;
  v->delta_pos = delta_pos;
  v->comp = c->comp;
  int natoms = 0;
  for (int i = 0; i < r->nlits; i++)
    if (r->lits[i]->kind == LIT_POS) natoms++;
  v->natoms = natoms;
  v->atom_rel = arena_alloc(&c->p->arena, (size_t)(natoms + 1) * sizeof *v->atom_rel);
  if (v->atom_rel == NULL) return err_oom(c->e);
  int k = 0;
  for (int i = 0; i < r->nlits; i++)
    if (r->lits[i]->kind == LIT_POS) v->atom_rel[k++] = r->lits[i]->atom.rel;
  if (plan_lits(c, r->lits, r->nlits, bound, delta_pos, true, &v->plan) != STATUS_OK) return STATUS_ERR;
  for (int i = 0; i < r->head.nargs; i++) {
    const Expr *x = r->head.args[i];
    if (x->kind == EX_WILD || !expr_bound(x, bound))
      return err_set(c->e, CODE_RULE_UNSAFE, r->file, r->line, "a head variable is not bound by a positive atom");
  }
  return STATUS_OK;
}

Status plan_program(Program *p, const Strata *s, Table *tabs, Variants *naive, Variants *delta, Err *e) {
  PCtx c = { p, s, tabs, NULL, 0, e };
  for (int comp = 0; comp < s->ncomps; comp++) {
    c.comp = comp;
    for (size_t i = 0; i < p->rules.n; i++) {
      Rule *r = &p->rules.p[i];
      if (s->comp[r->head.rel] != comp) continue;
      Variant *v = ARR_SLOT(&p->arena, *naive, e);
      if (v == NULL) return STATUS_ERR;
      if (plan_variant(&c, r, -1, v) != STATUS_OK) return STATUS_ERR;
      int pos = 0;
      for (int k = 0; k < r->nlits; k++) {
        if (r->lits[k]->kind != LIT_POS) continue;
        if (s->comp[r->lits[k]->atom.rel] == comp) {
          Variant *d = ARR_SLOT(&p->arena, *delta, e);
          if (d == NULL) return STATUS_ERR;
          if (plan_variant(&c, r, pos, d) != STATUS_OK) return STATUS_ERR;
        }
        pos++;
      }
    }
  }
  return STATUS_OK;
}
