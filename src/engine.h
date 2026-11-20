/* Semi-naive evaluation (E4, E7), budget (I4), provenance replay (E8, O6). */
#ifndef TUPU_ENGINE_H
#define TUPU_ENGINE_H
#include "plan.h"

typedef struct {
  uint32_t ds, cnt; /* delta start and row count at the start of the round */
} RelRange;

typedef struct {
  uint32_t rel, rule;
  size_t voff, boff;
} Entry;

typedef struct {
  Entry *e;
  size_t n, cap;
  I128 *vals;
  size_t nvals, vcap;
  uint32_t *bodies;
  size_t nbodies, bcap;
} Buf;

typedef enum { END_HEAD, END_AGG } PlanEnd;
typedef enum { MODE_DERIVE, MODE_REPLAY } ExecMode;

typedef struct {
  AggKind kind;
  I128 acc;
  bool any;
  const Expr *expr; /* NULL for count */
} Acc;

typedef struct Engine {
  Program *prog;
  Strata strata;
  Table *tabs;
  Variants naive, delta;
  uint64_t budget, added;
  bool prov;
  /* execution state */
  I128 *vars;
  uint32_t *body;
  RelRange *rr;
  Buf buf;
  const Variant *cur;
  uint32_t round;
  Acc *acc;
  Err *err;
  ExecMode mode;
  const I128 *target;
  const uint32_t *target_body;
  bool found;
  uint64_t rows_derived, probes, rounds; /* counters of the bench (E-performance) */
} Engine;

Status engine_open(Engine *x, Program *p, const char *facts_dir, uint64_t budget, bool prov, Err *e);
Status engine_eval(Engine *x, Err *e);
void engine_close(Engine *x);
/* O6: every row of every output relation derives again from its rule and its body rows against the final relations */
Status engine_replay(Engine *x, Err *e);
const Variant *engine_variant_of(const Engine *x, uint32_t rule);

#endif
