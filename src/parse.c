/* Parser: tokens -> Program. Disjunction is expanded here: the rule list holds only conjunctive rules. */
#include "ast.h"
#include "lex.h"
#include "num.h"
#include <stdlib.h>
#include <string.h>

void program_init(Program *p) {
  memset(p, 0, sizeof *p);
  sym_init(&p->syms);
}

void program_free(Program *p) {
  sym_free(&p->syms);
  arena_free(&p->arena);
}

typedef struct {
  Lit **p;
  size_t n, cap;
} Conj;
typedef struct {
  Conj *p;
  size_t n, cap;
} Dnf;

typedef struct {
  Program *prog;
  Arena *a;
  Token *t;
  size_t pos;
  Err *e;
  struct { Name *p; size_t n, cap; } vars;
} Parser;

static Token *cur(Parser *x) { return &x->t[x->pos]; }
static Token *peek(Parser *x, size_t k) {
  /* the token list ends with T_EOF; never look past it */
  size_t i = x->pos;
  while (k > 0 && x->t[i].kind != T_EOF) {
    i++;
    k--;
  }
  return &x->t[i];
}
static void next(Parser *x) {
  if (x->t[x->pos].kind != T_EOF) x->pos++;
}
static Status perr(Parser *x, const char *what) {
  Token *t = cur(x);
  return err_set(x->e, CODE_PARSE, t->file, t->line, "%s", what);
}
static bool is_word(const Token *t, const char *w) {
  size_t n = strlen(w);
  return t->kind == T_IDENT && t->n == n && memcmp(t->s, w, n) == 0;
}
static bool reserved(const Token *t) {
  return is_word(t, "count") || is_word(t, "sum") || is_word(t, "min") || is_word(t, "max") || is_word(t, "ord");
}
static Status expect(Parser *x, Tok k, const char *what) {
  if (cur(x)->kind != k) return perr(x, what);
  next(x);
  return STATUS_OK;
}

static Expr *new_expr(Parser *x, ExKind k, long line) {
  Expr *ex = arena_alloc(x->a, sizeof *ex);
  if (ex == NULL) {
    (void)err_oom(x->e);
    return NULL;
  }
  memset(ex, 0, sizeof *ex);
  ex->kind = k;
  ex->line = line;
  return ex;
}

static int var_index(Parser *x, const Token *t, bool *ok) {
  for (size_t i = 0; i < x->vars.n; i++)
    if (x->vars.p[i].n == t->n && memcmp(x->vars.p[i].s, t->s, t->n) == 0) return (int)i;
  Name *s = ARR_SLOT(x->a, x->vars, x->e);
  if (s == NULL) {
    *ok = false;
    return 0;
  }
  s->s = t->s;
  s->n = t->n;
  return (int)(x->vars.n - 1);
}

static Expr *parse_expr(Parser *x);

static Expr *parse_primary(Parser *x) {
  Token *t = cur(x);
  if (t->kind == T_NUMBER) {
    if (t->mag > (U128)NUM_MAX) {
      (void)perr(x, "number literal out of range");
      return NULL;
    }
    Expr *ex = new_expr(x, EX_NUM, t->line);
    if (ex == NULL) return NULL;
    ex->num = (I128)t->mag;
    next(x);
    return ex;
  }
  if (t->kind == T_STRING) {
    Expr *ex = new_expr(x, EX_SYM, t->line);
    if (ex == NULL) return NULL;
    if (sym_intern(&x->prog->syms, t->s, t->n, &ex->sym, x->e) != STATUS_OK) return NULL;
    next(x);
    return ex;
  }
  if (t->kind == T_LPAREN) {
    next(x);
    Expr *ex = parse_expr(x);
    if (ex == NULL) return NULL;
    if (expect(x, T_RPAREN, "expected )") != STATUS_OK) return NULL;
    return ex;
  }
  if (t->kind == T_IDENT) {
    bool fn = is_word(t, "max") || is_word(t, "min") || is_word(t, "ord");
    if (fn && peek(x, 1)->kind == T_LPAREN) {
      Expr *ex = new_expr(x, EX_FN, t->line);
      if (ex == NULL) return NULL;
      ex->fn = is_word(t, "max") ? FN_MAX : is_word(t, "min") ? FN_MIN : FN_ORD;
      next(x);
      next(x);
      ex->a = parse_expr(x);
      if (ex->a == NULL) return NULL;
      if (ex->fn != FN_ORD) {
        if (expect(x, T_COMMA, "expected , between the two arguments") != STATUS_OK) return NULL;
        ex->b = parse_expr(x);
        if (ex->b == NULL) return NULL;
      }
      if (expect(x, T_RPAREN, "expected )") != STATUS_OK) return NULL;
      return ex;
    }
    if (reserved(t)) {
      (void)perr(x, "reserved word used as a name");
      return NULL;
    }
    if (t->n == 1 && t->s[0] == '_') {
      Expr *ex = new_expr(x, EX_WILD, t->line);
      if (ex == NULL) return NULL;
      next(x);
      return ex;
    }
    Expr *ex = new_expr(x, EX_VAR, t->line);
    if (ex == NULL) return NULL;
    bool ok = true;
    ex->var = var_index(x, t, &ok);
    if (!ok) return NULL;
    next(x);
    return ex;
  }
  (void)perr(x, "expected an expression");
  return NULL;
}

static Expr *parse_unary(Parser *x) {
  Token *t = cur(x);
  if (t->kind == T_MINUS) {
    next(x);
    Token *n = cur(x);
    if (n->kind == T_NUMBER) { /* a negative literal; the magnitude may be 2^127 */
      Expr *ex = new_expr(x, EX_NUM, n->line);
      if (ex == NULL) return NULL;
      ex->num = (I128)(0 - n->mag);
      next(x);
      return ex;
    }
    Expr *a = parse_unary(x);
    if (a == NULL) return NULL;
    Expr *ex = new_expr(x, EX_NEG, t->line);
    if (ex == NULL) return NULL;
    ex->a = a;
    return ex;
  }
  return parse_primary(x);
}

static Expr *parse_term(Parser *x) {
  Expr *l = parse_unary(x);
  while (l != NULL && (cur(x)->kind == T_STAR || cur(x)->kind == T_SLASH || cur(x)->kind == T_PERCENT)) {
    Token *t = cur(x);
    next(x);
    Expr *r = parse_unary(x);
    if (r == NULL) return NULL;
    Expr *ex = new_expr(x, EX_BIN, t->line);
    if (ex == NULL) return NULL;
    ex->op = t->kind == T_STAR ? OP_MUL : t->kind == T_SLASH ? OP_DIV : OP_REM;
    ex->a = l;
    ex->b = r;
    l = ex;
  }
  return l;
}

static Expr *parse_expr(Parser *x) {
  Expr *l = parse_term(x);
  while (l != NULL && (cur(x)->kind == T_PLUS || cur(x)->kind == T_MINUS)) {
    Token *t = cur(x);
    next(x);
    Expr *r = parse_term(x);
    if (r == NULL) return NULL;
    Expr *ex = new_expr(x, EX_BIN, t->line);
    if (ex == NULL) return NULL;
    ex->op = t->kind == T_PLUS ? OP_ADD : OP_SUB;
    ex->a = l;
    ex->b = r;
    l = ex;
  }
  return l;
}

/* name ( args ): the arguments are expressions; the caller restricts them */
static Status parse_atom(Parser *x, Atom *at) {
  Token *t = cur(x);
  if (t->kind != T_IDENT) return perr(x, "expected a relation name");
  if (reserved(t)) return perr(x, "reserved word used as a relation name");
  at->name.s = t->s;
  at->name.n = t->n;
  at->rel = -1;
  at->line = t->line;
  at->args = NULL;
  at->nargs = 0;
  next(x);
  if (expect(x, T_LPAREN, "expected (") != STATUS_OK) return STATUS_ERR;
  struct { Expr **p; size_t n, cap; } args = { NULL, 0, 0 };
  if (cur(x)->kind != T_RPAREN) {
    for (;;) {
      Expr *ex = parse_expr(x);
      if (ex == NULL) return STATUS_ERR;
      Expr **s = ARR_SLOT(x->a, args, x->e);
      if (s == NULL) return STATUS_ERR;
      *s = ex;
      if (cur(x)->kind == T_COMMA) {
        next(x);
        continue;
      }
      break;
    }
  }
  if (expect(x, T_RPAREN, "expected )") != STATUS_OK) return STATUS_ERR;
  at->args = args.p;
  at->nargs = (int)args.n;
  return STATUS_OK;
}

static Status body_atom_args(Parser *x, const Atom *at) {
  for (int i = 0; i < at->nargs; i++) {
    ExKind k = at->args[i]->kind;
    if (k != EX_VAR && k != EX_WILD && k != EX_NUM && k != EX_SYM)
      return err_set(x->e, CODE_PARSE, x->t[x->pos].file, at->args[i]->line, "an expression as an argument of a body atom");
  }
  return STATUS_OK;
}

static Status parse_body(Parser *x, Dnf *out, bool top);

static Status parse_literal(Parser *x, Lit **out) {
  Token *t = cur(x);
  Lit *lit = arena_alloc(x->a, sizeof *lit);
  if (lit == NULL) return err_oom(x->e);
  memset(lit, 0, sizeof *lit);
  lit->line = t->line;
  *out = lit;
  if (t->kind == T_BANG) {
    next(x);
    lit->kind = LIT_NEG;
    if (parse_atom(x, &lit->atom) != STATUS_OK) return STATUS_ERR;
    return body_atom_args(x, &lit->atom);
  }
  if (t->kind == T_IDENT && peek(x, 1)->kind == T_LPAREN && !reserved(t)) {
    lit->kind = LIT_POS;
    if (parse_atom(x, &lit->atom) != STATUS_OK) return STATUS_ERR;
    return body_atom_args(x, &lit->atom);
  }
  lit->kind = LIT_CMP;
  lit->l = parse_expr(x);
  if (lit->l == NULL) return STATUS_ERR;
  Tok k = cur(x)->kind;
  switch (k) {
  case T_EQ: lit->cmp = CMP_EQ; break;
  case T_NE: lit->cmp = CMP_NE; break;
  case T_LT: lit->cmp = CMP_LT; break;
  case T_LE: lit->cmp = CMP_LE; break;
  case T_GT: lit->cmp = CMP_GT; break;
  case T_GE: lit->cmp = CMP_GE; break;
  default: return perr(x, "expected a comparison operator");
  }
  next(x);
  Token *r = cur(x);
  bool agg = k == T_EQ && r->kind == T_IDENT &&
             (is_word(r, "count") || is_word(r, "sum") ||
              ((is_word(r, "min") || is_word(r, "max")) && peek(x, 1)->kind != T_LPAREN));
  if (!agg) {
    lit->r = parse_expr(x);
    return lit->r == NULL ? STATUS_ERR : STATUS_OK;
  }
  if (lit->l->kind != EX_VAR) return perr(x, "the result of an aggregate must be a variable");
  lit->kind = LIT_AGG;
  lit->agg = is_word(r, "count") ? AGG_COUNT : is_word(r, "sum") ? AGG_SUM : is_word(r, "min") ? AGG_MIN : AGG_MAX;
  next(x);
  if (lit->agg != AGG_COUNT) {
    lit->aggexpr = parse_expr(x);
    if (lit->aggexpr == NULL) return STATUS_ERR;
  }
  if (expect(x, T_COLON, "expected : after the aggregate") != STATUS_OK) return STATUS_ERR;
  if (expect(x, T_LBRACE, "expected { after :") != STATUS_OK) return STATUS_ERR;
  Dnf d = { NULL, 0, 0 };
  if (parse_body(x, &d, false) != STATUS_OK) return STATUS_ERR;
  if (expect(x, T_RBRACE, "expected }") != STATUS_OK) return STATUS_ERR;
  if (d.n != 1) return perr(x, "disjunction inside an aggregate body");
  for (size_t i = 0; i < d.p[0].n; i++)
    if (d.p[0].p[i]->kind == LIT_AGG) return perr(x, "nested aggregate");
  lit->body = d.p[0].p;
  lit->nbody = (int)d.p[0].n;
  return STATUS_OK;
}

static Status dnf_one(Parser *x, Lit *lit, Dnf *d) {
  Conj *c = ARR_SLOT(x->a, *d, x->e);
  if (c == NULL) return STATUS_ERR;
  c->p = NULL;
  c->n = c->cap = 0;
  Lit **s = ARR_SLOT(x->a, *c, x->e);
  if (s == NULL) return STATUS_ERR;
  *s = lit;
  return STATUS_OK;
}

static Status parse_primary_body(Parser *x, Dnf *d) {
  if (cur(x)->kind == T_LPAREN) {
    /* try a parenthesised group of literals; if that does not read, the ( starts an expression of a comparison */
    size_t save = x->pos;
    Err keep = *x->e;
    next(x);
    Dnf inner = { NULL, 0, 0 };
    if (parse_body(x, &inner, false) == STATUS_OK && cur(x)->kind == T_RPAREN) {
      next(x);
      *d = inner;
      return STATUS_OK;
    }
    *x->e = keep;
    x->pos = save;
  }
  Lit *lit = NULL;
  if (parse_literal(x, &lit) != STATUS_OK) return STATUS_ERR;
  return dnf_one(x, lit, d);
}

static Status parse_and(Parser *x, Dnf *out) {
  Dnf acc = { NULL, 0, 0 };
  if (parse_primary_body(x, &acc) != STATUS_OK) return STATUS_ERR;
  while (cur(x)->kind == T_COMMA) {
    next(x);
    Dnf rhs = { NULL, 0, 0 };
    if (parse_primary_body(x, &rhs) != STATUS_OK) return STATUS_ERR;
    Dnf prod = { NULL, 0, 0 };
    for (size_t i = 0; i < acc.n; i++)
      for (size_t j = 0; j < rhs.n; j++) {
        Conj *c = ARR_SLOT(x->a, prod, x->e);
        if (c == NULL) return STATUS_ERR;
        c->p = NULL;
        c->n = c->cap = 0;
        for (size_t k = 0; k < acc.p[i].n; k++) {
          Lit **s = ARR_SLOT(x->a, *c, x->e);
          if (s == NULL) return STATUS_ERR;
          *s = acc.p[i].p[k];
        }
        for (size_t k = 0; k < rhs.p[j].n; k++) {
          Lit **s = ARR_SLOT(x->a, *c, x->e);
          if (s == NULL) return STATUS_ERR;
          *s = rhs.p[j].p[k];
        }
      }
    acc = prod;
  }
  *out = acc;
  return STATUS_OK;
}

static Status parse_body(Parser *x, Dnf *out, bool top) {
  (void)top;
  Dnf acc = { NULL, 0, 0 };
  if (parse_and(x, &acc) != STATUS_OK) return STATUS_ERR;
  while (cur(x)->kind == T_SEMI) {
    next(x);
    Dnf rhs = { NULL, 0, 0 };
    if (parse_and(x, &rhs) != STATUS_OK) return STATUS_ERR;
    for (size_t j = 0; j < rhs.n; j++) {
      Conj *c = ARR_SLOT(x->a, acc, x->e);
      if (c == NULL) return STATUS_ERR;
      *c = rhs.p[j];
    }
  }
  *out = acc;
  return STATUS_OK;
}

static Status parse_clause(Parser *x) {
  Program *pg = x->prog;
  Token *first = cur(x);
  x->vars.n = 0;
  Atom head;
  if (parse_atom(x, &head) != STATUS_OK) return STATUS_ERR;
  if (cur(x)->kind == T_DOT) { /* a program fact: every argument is a literal */
    for (int i = 0; i < head.nargs; i++) {
      ExKind k = head.args[i]->kind;
      if (k == EX_VAR || k == EX_WILD)
        return err_set(x->e, CODE_RULE_UNSAFE, first->file, first->line, "a fact with a variable or _");
      if (k != EX_NUM && k != EX_SYM) return err_set(x->e, CODE_PARSE, first->file, first->line, "an expression in a fact");
    }
    next(x);
    Fact *f = ARR_SLOT(x->a, pg->facts, x->e);
    if (f == NULL) return STATUS_ERR;
    f->file = first->file;
    f->line = first->line;
    f->atom = head;
    return STATUS_OK;
  }
  if (expect(x, T_IMPLIES, "expected :- or .") != STATUS_OK) return STATUS_ERR;
  Dnf d = { NULL, 0, 0 };
  if (parse_body(x, &d, true) != STATUS_OK) return STATUS_ERR;
  if (expect(x, T_DOT, "expected . at the end of a rule") != STATUS_OK) return STATUS_ERR;
  for (size_t i = 0; i < d.n; i++) {
    Rule *r = ARR_SLOT(x->a, pg->rules, x->e);
    if (r == NULL) return STATUS_ERR;
    r->file = first->file;
    r->line = first->line;
    r->head = head;
    r->lits = d.p[i].p;
    r->nlits = (int)d.p[i].n;
    r->nvars = (int)x->vars.n;
    r->index = (int)(pg->rules.n - 1);
  }
  return STATUS_OK;
}

static Status parse_decl(Parser *x) {
  Program *pg = x->prog;
  Token *d = cur(x);
  next(x);
  Token *nm = cur(x);
  if (nm->kind != T_IDENT) return perr(x, "expected a relation name");
  if (reserved(nm)) return perr(x, "reserved word used as a relation name");
  Relation *r = ARR_SLOT(x->a, pg->rels, x->e);
  if (r == NULL) return STATUS_ERR;
  memset(r, 0, sizeof *r);
  r->name.s = nm->s;
  r->name.n = nm->n;
  r->file = d->file;
  r->line = d->line;
  next(x);
  if (expect(x, T_LPAREN, "expected (") != STATUS_OK) return STATUS_ERR;
  if (cur(x)->kind != T_RPAREN) {
    for (;;) {
      if (cur(x)->kind != T_IDENT) return perr(x, "expected a column name");
      next(x);
      if (expect(x, T_COLON, "expected : after the column name") != STATUS_OK) return STATUS_ERR;
      Token *ty = cur(x);
      if (ty->kind != T_IDENT) return perr(x, "expected a type name");
      if (r->ncols >= MAXCOLS) return perr(x, "too many columns");
      r->coltype[r->ncols].s = ty->s;
      r->coltype[r->ncols].n = ty->n;
      r->ncols++;
      next(x);
      if (cur(x)->kind == T_COMMA) {
        next(x);
        continue;
      }
      break;
    }
  }
  if (expect(x, T_RPAREN, "expected )") != STATUS_OK) return STATUS_ERR;
  if (cur(x)->kind == T_IDENT && peek(x, 1)->kind != T_LPAREN) return perr(x, "a relation qualifier is not supported");
  return STATUS_OK;
}

static Status parse_type(Parser *x) {
  Program *pg = x->prog;
  Token *d = cur(x);
  next(x);
  Token *nm = cur(x);
  if (nm->kind != T_IDENT) return perr(x, "expected a type name");
  if (reserved(nm)) return perr(x, "reserved word used as a type name");
  TypeDef *t = ARR_SLOT(x->a, pg->types, x->e);
  if (t == NULL) return STATUS_ERR;
  memset(t, 0, sizeof *t);
  t->name.s = nm->s;
  t->name.n = nm->n;
  t->file = d->file;
  t->line = d->line;
  next(x);
  if (cur(x)->kind == T_SUBTYPE) {
    next(x);
    Token *b = cur(x);
    t->kind = TD_SUBTYPE;
    if (is_word(b, "symbol")) t->base = BASE_SYMBOL;
    else if (is_word(b, "number")) t->base = BASE_NUMBER;
    else return perr(x, "a subtype must be of symbol or number");
    next(x);
    return STATUS_OK;
  }
  if (cur(x)->kind == T_EQ) {
    next(x);
    t->kind = TD_UNION;
    struct { Name *p; size_t n, cap; } ms = { NULL, 0, 0 };
    for (;;) {
      Token *m = cur(x);
      if (m->kind != T_IDENT) return perr(x, "expected a type name");
      Name *s = ARR_SLOT(x->a, ms, x->e);
      if (s == NULL) return STATUS_ERR;
      s->s = m->s;
      s->n = m->n;
      next(x);
      if (cur(x)->kind == T_PIPE) {
        next(x);
        continue;
      }
      break;
    }
    t->members = ms.p;
    t->nmembers = (int)ms.n;
    return STATUS_OK;
  }
  return perr(x, "expected <: or = in a type definition");
}

static Status parse_io(Parser *x, bool input) {
  Program *pg = x->prog;
  next(x);
  for (;;) {
    Token *nm = cur(x);
    if (nm->kind != T_IDENT) return perr(x, "expected a relation name");
    Mention *m = input ? ARR_SLOT(x->a, pg->inputs, x->e) : ARR_SLOT(x->a, pg->outputs, x->e);
    if (m == NULL) return STATUS_ERR;
    m->name.s = nm->s;
    m->name.n = nm->n;
    m->file = nm->file;
    m->line = nm->line;
    next(x);
    if (cur(x)->kind == T_LPAREN) { /* options: empty, or IO=file, IO=stdout */
      next(x);
      if (cur(x)->kind != T_RPAREN) {
        if (!is_word(cur(x), "IO")) return perr(x, "only the option IO is supported");
        next(x);
        if (expect(x, T_EQ, "expected = after IO") != STATUS_OK) return STATUS_ERR;
        if (!is_word(cur(x), "file") && !is_word(cur(x), "stdout")) return perr(x, "IO must be file or stdout");
        next(x);
      }
      if (expect(x, T_RPAREN, "expected )") != STATUS_OK) return STATUS_ERR;
      return STATUS_OK;
    }
    if (cur(x)->kind == T_COMMA) {
      next(x);
      continue;
    }
    return STATUS_OK;
  }
}

Status parse_program(Program *p, const char *text, size_t len, const char *file0, Err *e) {
  Tokens toks = { NULL, 0, 0 };
  if (lex_program(&p->arena, text, len, file0, &toks, e) != STATUS_OK) return STATUS_ERR;
  Parser x = { p, &p->arena, toks.p, 0, e, { NULL, 0, 0 } };
  while (cur(&x)->kind != T_EOF) {
    Status st;
    switch (cur(&x)->kind) {
    case T_DECL: st = parse_decl(&x); break;
    case T_TYPE: st = parse_type(&x); break;
    case T_INPUT: st = parse_io(&x, true); break;
    case T_OUTPUT: st = parse_io(&x, false); break;
    case T_IDENT: st = parse_clause(&x); break;
    default: st = perr(&x, "expected a directive or a clause"); break;
    }
    if (st != STATUS_OK) return STATUS_ERR;
  }
  return STATUS_OK;
}
