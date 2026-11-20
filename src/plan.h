/* Join plans (E7, ruling S82 4). A plan is a fixed list of steps per rule variant; safety is decided here. */
#ifndef TUPU_PLAN_H
#define TUPU_PLAN_H
#include "check.h"
#include "table.h"

typedef enum { AS_CONST, AS_BOUND, AS_FREE, AS_EQ, AS_WILD } ArgKind;
typedef struct {
  ArgKind k;
  I128 val; /* AS_CONST */
  int var;  /* AS_BOUND, AS_FREE, AS_EQ */
} ArgSlot;

typedef enum { RANGE_FULL, RANGE_PREV, RANGE_DELTA } RangeKind;
typedef enum { ST_ATOM, ST_NEG, ST_FILTER, ST_BIND, ST_AGG } StepKind;

typedef struct Plan Plan;
typedef struct {
  StepKind kind;
  int rel;
  int nargs;
  ArgSlot *args;
  Index *idx;    /* NULL: scan, or a membership test for a negated atom with every column bound */
  uint32_t *cols;  /* bound columns, ascending: for a negated atom the columns that are not _ */
  uint32_t nbound;
  bool exact;    /* ST_NEG: no wildcard, so a set membership test decides */
  int atom_pos;  /* ST_ATOM at the top level: index among the positive atoms in text order, else -1 */
  RangeKind range;
  CmpOp cmp;
  const Expr *l, *r; /* ST_FILTER sides; ST_BIND: l is the term bound to var; ST_AGG: l is the aggregate expression */
  int var;           /* ST_BIND, ST_AGG: the variable that receives the value */
  bool target_bound; /* ST_AGG: the result variable is already bound: a filter */
  AggKind agg;
  Plan *sub; /* ST_AGG: the plan of the aggregate body */
  long line;
} Step;

struct Plan {
  Step *steps;
  int nsteps;
};

typedef struct {
  const Rule *rule;
  int delta_pos; /* text position of the delta atom, or -1 for the first round */
  int natoms;    /* positive atoms of the rule at the top level */
  int *atom_rel; /* relation of each, in text order */
  Plan plan;
  int comp;
} Variant;

typedef struct {
  Variant *p;
  size_t n, cap;
} Variants;

/* plan every rule: round-1 variants in `naive`, recursive-round variants in `delta`, each grouped by the component
   of the head and, inside it, in rule order */
Status plan_program(Program *p, const Strata *s, Table *tabs, Variants *naive, Variants *delta, Err *e);

#endif
