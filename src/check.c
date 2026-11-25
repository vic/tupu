#include "check.h"
#include <stdlib.h>
#include <string.h>

bool ts_empty(const Program *p, const TypeSet *t) {
  if (t->sym || t->num) return false;
  for (size_t i = 0; t->sub != NULL && i < p->tywords; i++)
    if (t->sub[i] != 0) return false;
  return true;
}

static uint64_t sub_word(const TypeSet *t, size_t i) { return t->sub == NULL ? 0 : t->sub[i]; }

/* the intersection of a and b, word i of the subtype part */
static uint64_t meet_word(const Program *p, const TypeSet *a, const TypeSet *b, size_t i) {
  uint64_t aw = sub_word(a, i), bw = sub_word(b, i);
  uint64_t s = aw & bw;
  if (b->sym) s |= aw & p->symmask[i];
  if (b->num) s |= aw & p->nummask[i];
  if (a->sym) s |= bw & p->symmask[i];
  if (a->num) s |= bw & p->nummask[i];
  return s;
}

/* a and b have a common member */
static bool ts_meets(const Program *p, const TypeSet *a, const TypeSet *b) {
  if ((a->sym && b->sym) || (a->num && b->num)) return true;
  for (size_t i = 0; i < p->tywords; i++)
    if (meet_word(p, a, b, i) != 0) return true;
  return false;
}

static bool name_eq(Name a, const char *s) { return a.n == strlen(s) && memcmp(a.s, s, a.n) == 0; }
static bool names_eq(Name a, Name b) { return a.n == b.n && memcmp(a.s, b.s, a.n) == 0; }

/* look up a type name: symbol, number, or a user type defined earlier */
static const TypeDef *find_type(const Program *p, size_t upto, Name n) {
  for (size_t i = 0; i < upto; i++)
    if (names_eq(p->types.p[i].name, n)) return &p->types.p[i];
  return NULL;
}

static Status resolve_type(Program *p, size_t upto, Name n, long line, const char *file, TypeSet *set, Base *base, Err *e) {
  memset(set, 0, sizeof *set);
  if (name_eq(n, "symbol")) {
    set->sym = true;
    *base = BASE_SYMBOL;
    return STATUS_OK;
  }
  if (name_eq(n, "number")) {
    set->num = true;
    *base = BASE_NUMBER;
    return STATUS_OK;
  }
  const TypeDef *t = find_type(p, upto, n);
  if (t == NULL) return err_set(e, CODE_UNRESOLVED_NAME, file, line, "unknown type %.*s", (int)n.n, n.s);
  *set = t->set;
  *base = t->setbase;
  return STATUS_OK;
}

static Status build_types(Program *p, Err *e) {
  size_t nsub = 0;
  for (size_t i = 0; i < p->types.n; i++)
    if (p->types.p[i].kind == TD_SUBTYPE) nsub++;
  if (nsub >= UINT32_MAX) return err_set(e, CODE_PARSE, NULL, 0, "too many subtypes");
  p->tywords = nsub == 0 ? 1 : (nsub + 63) / 64;
  p->symmask = arena_alloc(&p->arena, p->tywords * sizeof *p->symmask);
  p->nummask = arena_alloc(&p->arena, p->tywords * sizeof *p->nummask);
  if (p->symmask == NULL || p->nummask == NULL) return err_oom(e);
  memset(p->symmask, 0, p->tywords * sizeof *p->symmask);
  memset(p->nummask, 0, p->tywords * sizeof *p->nummask);
  for (size_t i = 0; i < p->types.n; i++) {
    TypeDef *t = &p->types.p[i];
    if (name_eq(t->name, "symbol") || name_eq(t->name, "number") || find_type(p, i, t->name) != NULL)
      return err_set(e, CODE_PARSE, t->file, t->line, "type %.*s is defined twice", (int)t->name.n, t->name.s);
    memset(&t->set, 0, sizeof t->set);
    t->set.sub = arena_alloc(&p->arena, p->tywords * sizeof *t->set.sub);
    if (t->set.sub == NULL) return err_oom(e);
    memset(t->set.sub, 0, p->tywords * sizeof *t->set.sub);
    if (t->kind == TD_SUBTYPE) {
      size_t k = (size_t)p->nsubtypes++;
      t->set.sub[k / 64] |= (uint64_t)1 << (k % 64);
      if (t->base == BASE_SYMBOL) p->symmask[k / 64] |= (uint64_t)1 << (k % 64);
      else p->nummask[k / 64] |= (uint64_t)1 << (k % 64);
      t->setbase = t->base;
    } else {
      bool first = true;
      for (int m = 0; m < t->nmembers; m++) {
        TypeSet ms;
        Base mb;
        if (resolve_type(p, i, t->members[m], t->line, t->file, &ms, &mb, e) != STATUS_OK) return STATUS_ERR;
        if (first) t->setbase = mb;
        else if (mb != t->setbase) return err_set(e, CODE_TYPE_MISMATCH, t->file, t->line, "a union mixes symbol and number types");
        first = false;
        t->set.sym = t->set.sym || ms.sym;
        t->set.num = t->set.num || ms.num;
        for (size_t w = 0; ms.sub != NULL && w < p->tywords; w++) t->set.sub[w] |= ms.sub[w];
      }
    }
  }
  return STATUS_OK;
}

static int find_rel(const Program *p, Name n) {
  for (size_t i = 0; i < p->rels.n; i++)
    if (names_eq(p->rels.p[i].name, n)) return (int)i;
  return -1;
}

static Status resolve_atom(Program *p, Atom *a, const char *file, Err *e) {
  a->rel = find_rel(p, a->name);
  if (a->rel < 0) return err_set(e, CODE_UNRESOLVED_NAME, file, a->line, "unknown relation %.*s", (int)a->name.n, a->name.s);
  const Relation *r = &p->rels.p[a->rel];
  if (a->nargs != r->ncols)
    return err_set(e, CODE_RELATION_ARITY, file, a->line, "relation %.*s has %d columns, the atom has %d", (int)a->name.n,
                   a->name.s, r->ncols, a->nargs);
  return STATUS_OK;
}

/* ---- types of a rule: one set per variable, narrowed by every place the variable stands ---- */
typedef struct {
  Program *p;
  TypeSet *vars;
  bool changed;
  const char *file;
  Err *e;
} TyCtx;

/* var := var meet with, in place; an empty result is a type mismatch */
static Status ty_narrow(TyCtx *c, int var, const TypeSet *with, long line) {
  const Program *p = c->p;
  TypeSet *v = &c->vars[var];
  bool nsym = v->sym && with->sym, nnum = v->num && with->num, any = nsym || nnum, changed = nsym != v->sym || nnum != v->num;
  for (size_t i = 0; i < p->tywords; i++) {
    uint64_t m = meet_word(p, v, with, i);
    if (m != v->sub[i]) changed = true;
    v->sub[i] = m;
    if (m != 0) any = true;
  }
  v->sym = nsym;
  v->num = nnum;
  if (!any) return err_set(c->e, CODE_TYPE_MISMATCH, c->file, line, "a variable has no common type in its columns");
  if (changed) c->changed = true;
  return STATUS_OK;
}

static TypeSet ts_num(void) {
  TypeSet t = { false, true, NULL };
  return t;
}
static TypeSet ts_sym(void) {
  TypeSet t = { true, false, NULL };
  return t;
}

static TypeSet ty_of(TyCtx *c, const Expr *x) {
  switch (x->kind) {
  case EX_NUM: return ts_num();
  case EX_SYM: return ts_sym();
  case EX_VAR: return c->vars[x->var];
  default: return ts_num();
  }
}

/* x is used where a value of set `req` is needed */
static Status ty_expr(TyCtx *c, const Expr *x, const TypeSet *req) {
  switch (x->kind) {
  case EX_WILD: return STATUS_OK;
  case EX_VAR: return ty_narrow(c, x->var, req, x->line);
  case EX_NUM:
  case EX_SYM: {
    TypeSet t = ty_of(c, x);
    if (!ts_meets(c->p, &t, req)) return err_set(c->e, CODE_TYPE_MISMATCH, c->file, x->line, "a literal does not fit its column");
    return STATUS_OK;
  }
  default: {
    TypeSet n = ts_num();
    if (!ts_meets(c->p, &n, req)) return err_set(c->e, CODE_TYPE_MISMATCH, c->file, x->line, "an arithmetic term does not fit its column");
    if (x->kind == EX_FN && x->fn == FN_ORD) {
      TypeSet s = ts_sym();
      return ty_expr(c, x->a, &s);
    }
    if (ty_expr(c, x->a, &n) != STATUS_OK) return STATUS_ERR;
    if (x->b != NULL) return ty_expr(c, x->b, &n);
    return STATUS_OK;
  }
  }
}

static Status ty_atom(TyCtx *c, const Atom *a) {
  const Relation *r = &c->p->rels.p[a->rel];
  for (int i = 0; i < a->nargs; i++)
    if (ty_expr(c, a->args[i], &r->colset[i]) != STATUS_OK) return STATUS_ERR;
  return STATUS_OK;
}

static Status ty_lit(TyCtx *c, const Lit *l) {
  switch (l->kind) {
  case LIT_POS:
  case LIT_NEG: return ty_atom(c, &l->atom);
  case LIT_CMP: {
    if (l->cmp == CMP_EQ || l->cmp == CMP_NE) {
      TypeSet a = ty_of(c, l->l), b = ty_of(c, l->r);
      if (!ts_meets(c->p, &a, &b)) return err_set(c->e, CODE_TYPE_MISMATCH, c->file, l->line, "the two sides of a comparison have no common type");
      if (ty_expr(c, l->l, &b) != STATUS_OK) return STATUS_ERR;
      /* the left side may have been narrowed: read the right side against the new left set */
      a = ty_of(c, l->l);
      return ty_expr(c, l->r, &a);
    }
    TypeSet n = ts_num();
    if (ty_expr(c, l->l, &n) != STATUS_OK) return STATUS_ERR;
    return ty_expr(c, l->r, &n);
  }
  case LIT_AGG: {
    TypeSet n = ts_num();
    if (ty_expr(c, l->l, &n) != STATUS_OK) return STATUS_ERR;
    if (l->aggexpr != NULL && ty_expr(c, l->aggexpr, &n) != STATUS_OK) return STATUS_ERR;
    for (int i = 0; i < l->nbody; i++)
      if (ty_lit(c, l->body[i]) != STATUS_OK) return STATUS_ERR;
    return STATUS_OK;
  }
  }
  return STATUS_OK;
}

static Status check_rule_types(Program *p, const Rule *r, Err *e) {
  size_t nv = (size_t)r->nvars;
  TypeSet *vars = arena_alloc(&p->arena, (nv + 1) * sizeof *vars);
  uint64_t *words = arena_alloc(&p->arena, (nv + 1) * p->tywords * sizeof *words);
  if (vars == NULL || words == NULL) return err_oom(e);
  for (size_t i = 0; i < nv; i++) {
    vars[i].sym = vars[i].num = true;
    vars[i].sub = words + i * p->tywords;
    for (size_t w = 0; w < p->tywords; w++) vars[i].sub[w] = p->symmask[w] | p->nummask[w];
  }
  TyCtx c = { p, vars, true, r->file, e };
  for (size_t pass = 0; c.changed && pass < nv + 3; pass++) {
    c.changed = false;
    if (ty_atom(&c, &r->head) != STATUS_OK) return STATUS_ERR;
    for (int i = 0; i < r->nlits; i++)
      if (ty_lit(&c, r->lits[i]) != STATUS_OK) return STATUS_ERR;
  }
  return STATUS_OK;
}

/* ---- lattice relations (subsumption) ----
   A lattice relation keeps one best row per key (the other columns): the largest or the smallest value of its lattice
   column. The table keeps only rows that improved the best of their key when they were derived; a dominated row
   (a worse value for a key that has a better one) may be missing. tupu admits a rule only when such a row can
   change nothing:
   - a lattice variable is the variable at the lattice column of a positive body atom of a lattice relation; it is
     "better" upward for a largest-kept relation and downward for a smallest-kept one;
   - a lattice variable stands nowhere else in any atom (no join, no key, no negation) and in no aggregate;
   - every comparison stays true when every lattice variable moves to a better value (polarity of left - right);
     = and != on a lattice variable are refused;
   - a head of a non-lattice relation holds no lattice variable; the lattice column of a lattice head moves to a
     better value of the head relation, or stays, when every lattice variable moves to a better value; its key
     columns hold no lattice variable;
   - an aggregate over a lattice relation is max (largest-kept) or min (smallest-kept) of the lattice column over that
     one atom; negation, count and sum over a lattice relation, and its .output, are refused.
   Then every derivation over all rows is matched by a derivation over the kept rows whose lattice inputs are at
   least as good: the non-lattice relations are equal and every key keeps the best value of all rows (monotone
   fixpoint over a lattice, Tarski 1955; the same argument as Flix, Madsen et al. PLDI 2016). */
typedef enum { POL_ZERO, POL_UP, POL_DOWN, POL_ANY } Pol;

static bool expr_uses(const Expr *x, int var) {
  if (x == NULL) return false;
  if (x->kind == EX_VAR) return x->var == var;
  return expr_uses(x->a, var) || expr_uses(x->b, var);
}

static Pol pol_flip(Pol p) { return p == POL_UP ? POL_DOWN : (p == POL_DOWN ? POL_UP : p); }

static Pol pol_add(Pol a, Pol b) {
  if (a == POL_ZERO) return b;
  if (b == POL_ZERO) return a;
  return a == b ? a : POL_ANY;
}

/* how x moves when var moves up */
static Pol expr_pol(const Expr *x, int var) {
  switch (x->kind) {
  case EX_VAR: return x->var == var ? POL_UP : POL_ZERO;
  case EX_NUM:
  case EX_SYM:
  case EX_WILD: return POL_ZERO;
  case EX_NEG: return pol_flip(expr_pol(x->a, var));
  case EX_BIN:
    if (x->op == OP_ADD) return pol_add(expr_pol(x->a, var), expr_pol(x->b, var));
    if (x->op == OP_SUB) return pol_add(expr_pol(x->a, var), pol_flip(expr_pol(x->b, var)));
    return expr_uses(x, var) ? POL_ANY : POL_ZERO;
  case EX_FN: /* max and min are non-decreasing in both arguments; ord is not monotone */
    if (x->fn == FN_MAX || x->fn == FN_MIN) return pol_add(expr_pol(x->a, var), expr_pol(x->b, var));
    return expr_uses(x, var) ? POL_ANY : POL_ZERO;
  }
  return POL_ANY;
}

/* two structurally equal expressions: their comparison does not depend on any value */
static bool expr_same(const Expr *a, const Expr *b) {
  if (a == NULL || b == NULL) return a == b;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
  case EX_NUM: return a->num == b->num;
  case EX_SYM: return a->sym == b->sym;
  case EX_VAR: return a->var == b->var;
  case EX_WILD: return false;
  case EX_NEG: return expr_same(a->a, b->a);
  case EX_BIN: return a->op == b->op && expr_same(a->a, b->a) && expr_same(a->b, b->b);
  case EX_FN: return a->fn == b->fn && expr_same(a->a, b->a) && expr_same(a->b, b->b);
  }
  return false;
}

static Status lattice_refuse(const Program *p, const Rule *r, int rel, const char *why, Err *e) {
  const Relation *R = &p->rels.p[rel];
  return err_set(e, CODE_PARSE, r->file, r->line, "lattice relation %.*s: %s", (int)R->name.n, R->name.s, why);
}

typedef struct {
  int var;
  Pol better; /* POL_UP or POL_DOWN */
  int rel;
  const Lit *at;
} LatVar;

static Status check_lattice_rule(const Program *p, const Rule *r, Err *e) {
  LatVar lv[r->nlits + 1]; /* one entry per body literal at most */
  int nlv = 0;
  for (int k = 0; k < r->nlits; k++) {
    const Lit *l = r->lits[k];
    if ((l->kind == LIT_POS || l->kind == LIT_NEG) && p->rels.p[l->atom.rel].lat_col >= 0) {
      const Relation *R = &p->rels.p[l->atom.rel];
      if (l->kind == LIT_NEG) return lattice_refuse(p, r, l->atom.rel, "read under negation", e);
      const Expr *x = l->atom.args[R->lat_col];
      if (x->kind != EX_VAR) return lattice_refuse(p, r, l->atom.rel, "the lattice column of a body atom must be a variable", e);
      lv[nlv++] = (LatVar){x->var, R->lat_max ? POL_UP : POL_DOWN, l->atom.rel, l};
    } else if (l->kind == LIT_AGG) {
      for (int b = 0; b < l->nbody; b++) {
        const Lit *a = l->body[b];
        if (a->kind != LIT_POS && a->kind != LIT_NEG) continue;
        const Relation *R = &p->rels.p[a->atom.rel];
        if (R->lat_col < 0) continue;
        bool same_dir = (l->agg == AGG_MAX && R->lat_max) || (l->agg == AGG_MIN && !R->lat_max);
        const Expr *v = a->atom.args[R->lat_col];
        if (a->kind == LIT_NEG || !same_dir || l->nbody != 1 || l->aggexpr == NULL || l->aggexpr->kind != EX_VAR ||
            v->kind != EX_VAR || l->aggexpr->var != v->var)
          return lattice_refuse(p, r, a->atom.rel, "an aggregate must be max (or min) of the lattice column over that one atom", e);
      }
    }
  }
  for (int i = 0; i < nlv; i++) {
    int var = lv[i].var;
    for (int k = 0; k < r->nlits; k++) {
      const Lit *l = r->lits[k];
      if (l->kind == LIT_POS || l->kind == LIT_NEG) {
        const Relation *R = &p->rels.p[l->atom.rel];
        for (int c = 0; c < l->atom.nargs; c++) {
          if (l == lv[i].at && c == R->lat_col) continue;
          if (expr_uses(l->atom.args[c], var)) return lattice_refuse(p, r, lv[i].rel, "a lattice variable is joined or used as a key", e);
        }
      } else if (l->kind == LIT_AGG) {
        bool uses = expr_uses(l->l, var) || expr_uses(l->aggexpr, var);
        for (int b = 0; b < l->nbody && !uses; b++) {
          const Lit *a = l->body[b];
          if (a->kind == LIT_POS || a->kind == LIT_NEG)
            for (int c = 0; c < a->atom.nargs; c++) uses = uses || expr_uses(a->atom.args[c], var);
          else if (a->kind == LIT_CMP)
            uses = uses || expr_uses(a->l, var) || expr_uses(a->r, var);
        }
        if (uses) return lattice_refuse(p, r, lv[i].rel, "a lattice variable is used in an aggregate", e);
      } else if (l->kind == LIT_CMP) {
        if (expr_same(l->l, l->r)) continue; /* a placeholder like x = x: constant truth */
        Pol d = pol_add(expr_pol(l->l, var), pol_flip(expr_pol(l->r, var))); /* how left - right moves as var moves up */
        if (lv[i].better == POL_DOWN) d = pol_flip(d);                         /* ... as var moves to a better value */
        bool keeps;
        switch (l->cmp) {
        case CMP_LT:
        case CMP_LE: keeps = d == POL_ZERO || d == POL_DOWN; break;
        case CMP_GT:
        case CMP_GE: keeps = d == POL_ZERO || d == POL_UP; break;
        default: keeps = d == POL_ZERO; break;
        }
        if (!keeps) return lattice_refuse(p, r, lv[i].rel, "a comparison may turn false when a lattice value improves", e);
      }
    }
    const Relation *H = &p->rels.p[r->head.rel];
    for (int c = 0; c < r->head.nargs; c++) {
      const Expr *x = r->head.args[c];
      if (H->lat_col == c) {
        Pol d = expr_pol(x, var);
        if (lv[i].better == POL_DOWN) d = pol_flip(d);
        Pol want = H->lat_max ? POL_UP : POL_DOWN;
        if (d != POL_ZERO && d != want)
          return lattice_refuse(p, r, lv[i].rel, "the head's lattice value must improve or stay when a lattice value improves", e);
      } else if (expr_uses(x, var)) {
        return lattice_refuse(p, r, lv[i].rel, "a lattice variable stands in a head key or a non-lattice head", e);
      }
    }
  }
  return STATUS_OK;
}

static Status check_lattices(Program *p, Err *e) {
  for (size_t i = 0; i < p->subsumes.n; i++) {
    const Subsume *s = &p->subsumes.p[i];
    int rel = find_rel(p, s->rel);
    if (rel < 0) return err_set(e, CODE_UNRESOLVED_NAME, s->file, s->line, "subsumption of unknown relation %.*s", (int)s->rel.n, s->rel.s);
    Relation *R = &p->rels.p[rel];
    if (R->ncols != s->ncols) return err_set(e, CODE_RELATION_ARITY, s->file, s->line, "subsumption arity differs from the declaration");
    if (R->lat_col >= 0) return err_set(e, CODE_PARSE, s->file, s->line, "a second subsumption of %.*s", (int)R->name.n, R->name.s);
    if (R->colkind[s->col] != BASE_NUMBER) return err_set(e, CODE_TYPE_MISMATCH, s->file, s->line, "the lattice column must be a number");
    if (R->is_output) return err_set(e, CODE_PARSE, s->file, s->line, "a lattice relation cannot be an .output");
    R->lat_col = s->col;
    R->lat_max = s->keep_max;
  }
  for (size_t i = 0; i < p->rules.n; i++)
    if (check_lattice_rule(p, &p->rules.p[i], e) != STATUS_OK) return STATUS_ERR;
  return STATUS_OK;
}

Status check_program(Program *p, Err *e) {
  if (build_types(p, e) != STATUS_OK) return STATUS_ERR;
  for (size_t i = 0; i < p->rels.n; i++) {
    Relation *r = &p->rels.p[i];
    for (size_t j = 0; j < i; j++)
      if (names_eq(p->rels.p[j].name, r->name))
        return err_set(e, CODE_PARSE, r->file, r->line, "relation %.*s is declared twice", (int)r->name.n, r->name.s);
    r->colset = arena_alloc(&p->arena, ((size_t)r->ncols + 1) * sizeof *r->colset);
    r->colkind = arena_alloc(&p->arena, ((size_t)r->ncols + 1) * sizeof *r->colkind);
    if (r->colset == NULL || r->colkind == NULL) return err_oom(e);
    for (int c = 0; c < r->ncols; c++) {
      Base b;
      if (resolve_type(p, p->types.n, r->coltype[c], r->line, r->file, &r->colset[c], &b, e) != STATUS_OK) return STATUS_ERR;
      r->colkind[c] = b;
    }
  }
  for (size_t i = 0; i < p->inputs.n; i++) {
    Mention *m = &p->inputs.p[i];
    int rel = find_rel(p, m->name);
    if (rel < 0) return err_set(e, CODE_UNRESOLVED_NAME, m->file, m->line, ".input of unknown relation %.*s", (int)m->name.n, m->name.s);
    p->rels.p[rel].is_input = true;
  }
  for (size_t i = 0; i < p->outputs.n; i++) {
    Mention *m = &p->outputs.p[i];
    int rel = find_rel(p, m->name);
    if (rel < 0) return err_set(e, CODE_UNRESOLVED_NAME, m->file, m->line, ".output of unknown relation %.*s", (int)m->name.n, m->name.s);
    p->rels.p[rel].is_output = true;
  }
  for (size_t i = 0; i < p->facts.n; i++) {
    Fact *f = &p->facts.p[i];
    if (resolve_atom(p, &f->atom, f->file, e) != STATUS_OK) return STATUS_ERR;
    const Relation *r = &p->rels.p[f->atom.rel];
    for (int c = 0; c < f->atom.nargs; c++) {
      const Expr *x = f->atom.args[c];
      bool sym = x->kind == EX_SYM;
      if (sym != (r->colkind[c] == BASE_SYMBOL)) return err_set(e, CODE_TYPE_MISMATCH, f->file, f->line, "a fact literal does not fit its column");
    }
  }
  for (size_t i = 0; i < p->rules.n; i++) {
    Rule *r = &p->rules.p[i];
    if (resolve_atom(p, &r->head, r->file, e) != STATUS_OK) return STATUS_ERR;
    for (int k = 0; k < r->nlits; k++) {
      Lit *l = r->lits[k];
      if (l->kind == LIT_POS || l->kind == LIT_NEG) {
        if (resolve_atom(p, &l->atom, r->file, e) != STATUS_OK) return STATUS_ERR;
      } else if (l->kind == LIT_AGG) {
        for (int b = 0; b < l->nbody; b++)
          if (l->body[b]->kind == LIT_POS || l->body[b]->kind == LIT_NEG)
            if (resolve_atom(p, &l->body[b]->atom, r->file, e) != STATUS_OK) return STATUS_ERR;
      }
    }
  }
  for (size_t i = 0; i < p->rules.n; i++)
    if (check_rule_types(p, &p->rules.p[i], e) != STATUS_OK) return STATUS_ERR;
  return check_lattices(p, e);
}

/* ---- stratification: iterative Tarjan over the relation dependency graph (head -> body relation) ---- */
typedef struct {
  int to;
  bool negative; /* a negated atom or an aggregate body */
  const Rule *rule;
} Edge;

void strata_free(Strata *s) {
  free(s->comp);
  free(s->recursive);
  s->comp = NULL;
  s->recursive = NULL;
}

Status stratify(Program *p, Strata *s, Err *e) {
  size_t n = p->rels.n;
  memset(s, 0, sizeof *s);
  /* edges in declaration order of the head relation, then rule order, then body order */
  size_t ne = 0, cap = 16;
  Edge *edges = malloc(cap * sizeof *edges);
  int *from = malloc(cap * sizeof *from);
  if (edges == NULL || from == NULL) {
    free(edges);
    free(from);
    return err_oom(e);
  }
  for (size_t h = 0; h < n; h++)
    for (size_t ri = 0; ri < p->rules.n; ri++) {
      const Rule *r = &p->rules.p[ri];
      if (r->head.rel != (int)h) continue;
      for (int k = 0; k < r->nlits; k++) {
        const Lit *l = r->lits[k];
        int nb = l->kind == LIT_AGG ? l->nbody : 1;
        for (int b = 0; b < nb; b++) {
          const Lit *a = l->kind == LIT_AGG ? l->body[b] : l;
          if (a->kind != LIT_POS && a->kind != LIT_NEG) continue;
          if (ne == cap) {
            cap *= 2;
            Edge *ne2 = realloc(edges, cap * sizeof *edges);
            int *nf2 = realloc(from, cap * sizeof *from);
            if (ne2 != NULL) edges = ne2;
            if (nf2 != NULL) from = nf2;
            if (ne2 == NULL || nf2 == NULL) {
              free(edges);
              free(from);
              return err_oom(e);
            }
          }
          edges[ne].to = a->atom.rel;
          edges[ne].negative = a->kind == LIT_NEG || l->kind == LIT_AGG;
          edges[ne].rule = r;
          from[ne] = (int)h;
          ne++;
        }
      }
    }
  /* adjacency by counting sort on `from`, keeping edge order */
  size_t *start = calloc(n + 1, sizeof *start);
  size_t *order = malloc((ne + 1) * sizeof *order);
  int *index = malloc((n + 1) * sizeof *index), *low = malloc((n + 1) * sizeof *low);
  int *stack = malloc((n + 1) * sizeof *stack), *comp = malloc((n + 1) * sizeof *comp);
  bool *onstack = calloc(n + 1, 1);
  size_t *cs = malloc((n + 1) * sizeof *cs); /* call stack: node, next edge position */
  size_t *ce = malloc((n + 1) * sizeof *ce);
  bool *rec = calloc(n + 1, 1);
  if (!start || !order || !index || !low || !stack || !comp || !onstack || !cs || !ce || !rec) {
    free(edges); free(from); free(start); free(order); free(index); free(low); free(stack); free(comp); free(onstack);
    free(cs); free(ce); free(rec);
    return err_oom(e);
  }
  for (size_t i = 0; i < ne; i++) start[from[i] + 1]++;
  for (size_t i = 0; i < n; i++) start[i + 1] += start[i];
  {
    size_t *fill = calloc(n + 1, sizeof *fill);
    if (fill == NULL) {
      free(edges); free(from); free(start); free(order); free(index); free(low); free(stack); free(comp); free(onstack);
      free(cs); free(ce); free(rec);
      return err_oom(e);
    }
    for (size_t i = 0; i < ne; i++) order[start[from[i]] + fill[from[i]]++] = i;
    free(fill);
  }
  for (size_t i = 0; i < n; i++) {
    index[i] = -1;
    comp[i] = -1;
  }
  int counter = 0, ncomp = 0;
  size_t sp = 0, cp = 0;
  for (size_t root = 0; root < n; root++) {
    if (index[root] >= 0) continue;
    cs[cp] = root;
    ce[cp] = start[root];
    cp++;
    index[root] = low[root] = counter++;
    stack[sp++] = (int)root;
    onstack[root] = true;
    while (cp > 0) {
      size_t v = cs[cp - 1];
      if (ce[cp - 1] < start[v + 1]) {
        const Edge *ed = &edges[order[ce[cp - 1]++]];
        size_t w = (size_t)ed->to;
        if (index[w] < 0) {
          index[w] = low[w] = counter++;
          stack[sp++] = (int)w;
          onstack[w] = true;
          cs[cp] = w;
          ce[cp] = start[w];
          cp++;
        } else if (onstack[w] && index[w] < low[v]) {
          low[v] = index[w];
        }
      } else {
        if (low[v] == index[v]) {
          for (;;) {
            int w = stack[--sp];
            onstack[w] = false;
            comp[w] = ncomp;
            if ((size_t)w == v) break;
          }
          ncomp++;
        }
        cp--;
        if (cp > 0) {
          size_t u = cs[cp - 1];
          if (low[v] < low[u]) low[u] = low[v];
        }
      }
    }
  }
  Status st = STATUS_OK;
  for (size_t i = 0; i < ne && st == STATUS_OK; i++) {
    if (comp[from[i]] != comp[edges[i].to]) continue;
    if (edges[i].negative) {
      const Relation *a = &p->rels.p[from[i]], *b = &p->rels.p[edges[i].to];
      st = err_set(e, CODE_NEGATION_CYCLE, edges[i].rule->file, edges[i].rule->line,
                   "%.*s depends through negation or an aggregate on %.*s in its own component", (int)a->name.n, a->name.s,
                   (int)b->name.n, b->name.s);
    } else {
      rec[from[i]] = true;
    }
  }
  for (size_t i = 0; i < n; i++) p->rels.p[i].comp = comp[i];
  free(edges); free(from); free(start); free(order); free(index); free(low); free(stack); free(onstack);
  free(cs); free(ce);
  if (st != STATUS_OK) {
    free(comp);
    free(rec);
    return st;
  }
  s->ncomps = ncomp;
  s->comp = comp;
  s->recursive = rec;
  return STATUS_OK;
}
