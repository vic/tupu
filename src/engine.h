/* Semi-naive evaluation (E4, E7), budget (I4), provenance replay (E8, O6). */
#ifndef TUPU_ENGINE_H
#define TUPU_ENGINE_H
#include "plan.h"

typedef struct {
  uint32_t ds, cnt; /* delta start and row count at the start of the round */
} RelRange;

typedef struct {
  size_t n, cap;
  uint64_t *hash;  /* per row: the hash of its cells (F3) */
  uint8_t *cells;  /* per row: rowbytes bytes, each cell at its column width (F2) */
  size_t rowbytes;
  uint32_t *bodies; /* per row: natoms body row ids, only with -P (E8) */
  uint32_t *hs; /* F1: open-addressing set of the rows this job buffered: entry index per slot, one used byte per slot, load factor 1/2 */
  uint8_t *hu;
  size_t hcap;
} Buf;

typedef enum { END_HEAD, END_AGG } PlanEnd;
typedef enum { MODE_DERIVE, MODE_REPLAY } ExecMode;

typedef struct {
  AggKind kind;
  I128 acc;
  bool any;
  const Expr *expr; /* NULL for count */
} Acc;

typedef struct {
  const Variant *v;
  int step; /* the first atom step of the plan, or -1 */
  uint32_t lo, hi;
  Buf buf;
  Err err;
  Status st;
  size_t seq; /* order of completion, used only by the MERGEORDER mutant */
} Job;

typedef struct JobList {
  Job *p;
  size_t n, cap;
} JobList;

struct ThreadPool;

typedef struct Engine {
  Program *prog;
  Strata strata;
  Table *tabs;
  Variants naive, delta;
  uint64_t budget, added;
  bool prov;
  /* execution state */
  I128 *vars;
  uint8_t *kbuf; /* packed index key scratch */
  I128 *scratch, *hv, *cells; /* one row of cells each: lookup keys, head rows, program facts and replay */
  uint32_t *body;
  RelRange *rr;
  Buf buf;
  const Variant *cur;
  uint32_t round;
  Acc *acc;
  Err *err;
  size_t nthreads;
  struct ThreadPool *tpool;
  JobList jl;
  size_t nvars_max, ncols_max, natoms_max;
  int chunk_step; /* S80: the plan step whose range this job cuts, or -1 */
  uint32_t chunk_lo, chunk_hi;
  ExecMode mode;
  const I128 *target;
  const uint32_t *target_body;
  bool found;
  uint64_t derivations, rows_derived, probes, rounds; /* counters of the bench (E-performance) */
#ifdef TUPU_COUNTERS
  uint64_t *rel_derived;           /* per relation: rows buffered by rule jobs (sums to rows_derived) */
  uint64_t *comp_rounds, *comp_ns; /* per stratum: rounds run and wall ns */
#endif
} Engine;

Status engine_open(Engine *x, Program *p, const char *facts_dir, uint64_t budget, bool prov, size_t nthreads, Err *e);
Status engine_open_mem(Engine *x, Program *p, uint64_t budget, size_t nthreads, Err *e);
Status engine_reset(Engine *x, Err *e);
Status engine_eval(Engine *x, Err *e);
void engine_close(Engine *x);
/* O6: every row of every output relation derives again from its rule and its body rows against the final relations */
Status engine_replay(Engine *x, Err *e);
const Variant *engine_variant_of(const Engine *x, uint32_t rule);
#ifdef TUPU_COUNTERS
/* stderr lines `rel NAME rows=N derived=M` (by name) and `stratum I rounds=R ns=T` (by index) */
void engine_print_counters(const Engine *x);
#endif

#endif
