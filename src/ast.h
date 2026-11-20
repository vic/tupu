/* The program after reading: types, relations, program facts and conjunctive rules (I1). */
#ifndef TUPU_AST_H
#define TUPU_AST_H
#include "common.h"
#include "sym.h"

#define MAXCOLS 16
#define TYPE_WORDS 4 /* at most 256 subtypes */

typedef enum { BASE_SYMBOL, BASE_NUMBER } Base;

/* a type is a set of values: all symbols, all numbers, or a set of subtypes (each a subset of one base) */
typedef struct {
  bool sym, num;
  uint64_t sub[TYPE_WORDS];
} TypeSet;

typedef struct {
  const char *s;
  size_t n;
} Name;

typedef enum { EX_NUM, EX_SYM, EX_VAR, EX_WILD, EX_NEG, EX_BIN, EX_FN } ExKind;
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM } BinOp;
typedef enum { FN_MAX, FN_MIN, FN_ORD } Fn;

typedef struct Expr {
  ExKind kind;
  long line;
  I128 num;      /* EX_NUM */
  uint32_t sym;  /* EX_SYM: the interned id */
  int var;       /* EX_VAR: index in the clause */
  BinOp op;      /* EX_BIN */
  Fn fn;         /* EX_FN */
  struct Expr *a, *b; /* operands; EX_NEG and ord use a only */
} Expr;

typedef struct {
  Name name;
  int rel; /* index into Program.rels once resolved, else -1 */
  Expr **args;
  int nargs;
  long line;
} Atom;

typedef enum { LIT_POS, LIT_NEG, LIT_CMP, LIT_AGG } LitKind;
typedef enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE } CmpOp;
typedef enum { AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX } AggKind;

typedef struct Lit {
  LitKind kind;
  long line;
  Atom atom;       /* LIT_POS, LIT_NEG */
  CmpOp cmp;       /* LIT_CMP */
  Expr *l, *r;     /* LIT_CMP; LIT_AGG: l is the result variable */
  AggKind agg;     /* LIT_AGG */
  Expr *aggexpr;   /* LIT_AGG: NULL for count */
  struct Lit **body;
  int nbody;
} Lit;

typedef struct {
  const char *file;
  long line;
  Atom head;
  Lit **lits;
  int nlits;
  int nvars;
  int index; /* position in reader order after disjunction expansion */
} Rule;

typedef struct {
  const char *file;
  long line;
  Atom atom; /* every argument is EX_NUM or EX_SYM */
} Fact;

typedef struct {
  Name name;
  const char *file;
  long line;
  int ncols;
  Name coltype[MAXCOLS];
  /* filled by check.c */
  TypeSet colset[MAXCOLS];
  Base colkind[MAXCOLS];
  bool is_input, is_output;
  int comp;
} Relation;

typedef enum { TD_SUBTYPE, TD_UNION } TypeDefKind;
typedef struct {
  Name name;
  long line;
  const char *file;
  TypeDefKind kind;
  Base base;     /* TD_SUBTYPE */
  Name *members; /* TD_UNION */
  int nmembers;
  TypeSet set; /* filled by check.c */
  Base setbase;
} TypeDef;

typedef struct {
  Name name;
  const char *file;
  long line;
} Mention; /* a relation named by .input or .output */

typedef struct {
  Arena arena;
  SymTab syms;
  struct { Relation *p; size_t n, cap; } rels;
  struct { TypeDef *p; size_t n, cap; } types;
  struct { Mention *p; size_t n, cap; } inputs, outputs;
  struct { Rule *p; size_t n, cap; } rules;
  struct { Fact *p; size_t n, cap; } facts;
  uint64_t symmask[TYPE_WORDS], nummask[TYPE_WORDS]; /* subtypes of each base */
  int nsubtypes;
} Program;

void program_init(Program *p);
void program_free(Program *p);

/* read the mcpp output text (line markers set file and line); file0 is used before the first marker */
Status parse_program(Program *p, const char *text, size_t len, const char *file0, Err *e);

#endif
