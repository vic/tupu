/* Tables (E3), dedup sets (E5), indexes (E6) and the facts reader (ruling S82 2, 8, 10). */
#ifndef TUPU_TABLE_H
#define TUPU_TABLE_H
#include "ast.h"

typedef enum { ORG_INPUT, ORG_FACT, ORG_RULE } Origin;

typedef struct {
  uint32_t mask; /* bound columns */
  uint64_t *hash;
  uint32_t *first;
  uint8_t *used;
  size_t scap, nslots;
  uint32_t *next; /* chain, newest to oldest; the last row of a chain points to itself */
  size_t next_cap;
  uint32_t synced; /* rows [0, synced) are in the index */
} Index;

typedef struct {
  int ncols;
  Base kind[MAXCOLS];
  uint32_t count, cap;
  void *col[MAXCOLS]; /* uint32_t symbol ids or I128 */
  uint8_t *org_kind;
  uint32_t *org_ref; /* rule index, program fact index, or 0 for input rows */
  uint32_t *round;
  uint32_t *dslot;
  uint8_t *dused;
  size_t dcap;
  Index **idx;
  int nidx, idxcap;
  bool want_prov;
  size_t *poff; /* per row: offset of its body row ids in pool */
  uint32_t *pool;
  size_t pool_n, pool_cap;
} Table;

static inline I128 tcell(const Table *t, int c, uint32_t row) {
  return t->kind[c] == BASE_SYMBOL ? (I128)((const uint32_t *)t->col[c])[row] : ((const I128 *)t->col[c])[row];
}

void table_init(Table *t, const Relation *r, bool want_prov);
void table_free(Table *t);
/* add a row (dedup): *added tells whether it was new. A table past 2^32-2 rows is datalog-budget. */
Status table_add(Table *t, const I128 *vals, Origin org, uint32_t ref, uint32_t round, const uint32_t *body, int nbody,
                 bool *added, Err *e);
bool table_contains(const Table *t, const I128 *vals);

uint64_t hash_cells(const Table *t, uint32_t mask, const I128 *vals_by_col);
Status table_index(Table *t, uint32_t mask, Index **out, Err *e);
Status index_sync(Index *ix, const Table *t, Err *e);
/* first row of the chain of this hash; false when none */
bool index_find(const Index *ix, uint64_t hash, uint32_t *row);

/* load DIR/<relation>.facts into the table (S82 2) */
Status facts_load(Table *t, const Relation *r, SymTab *syms, const char *path, Err *e);

#endif
