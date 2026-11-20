/* Tables (E3), dedup sets (E5), indexes (E6) and the facts reader (rulings S82 2, 8, 10). */
#ifndef TUPU_TABLE_H
#define TUPU_TABLE_H
#include "ast.h"

typedef enum { ORG_INPUT, ORG_FACT, ORG_RULE } Origin;

/* S102 covering index: key (the bound columns) -> bucket. A bucket holds, in ascending row-id order, the row ids (u32) and the
   non-bound columns of each row at their width (u32 symbol, I128 number), in ONE block: ids first, then the cells. */
typedef struct {
  uint8_t *blk; /* cap row ids, then cap rows of fbytes cells */
  uint32_t n, cap;
} Bucket;

typedef struct {
  uint32_t *cols; /* the bound columns, ascending */
  uint32_t nb;
  uint32_t *fcols; /* the other columns, ascending */
  uint32_t nf;
  size_t kbytes, fbytes; /* packed width of a key, of the free cells of one row */
  uint64_t *hash; /* open addressing: slot -> bucket, one used byte per slot, load factor at most 1/2 */
  uint32_t *bk;
  uint8_t *used;
  size_t scap;
  Bucket *bks;
  uint8_t *keys; /* kbytes per bucket */
  uint32_t nbk, bkcap;
  uint32_t synced; /* rows [0, synced) are in the index */
} Index;

typedef struct {
  int ncols;
  Base *kind;
  uint32_t count, cap;
  void **col; /* per column: uint32_t symbol ids or I128 */
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

Status table_init(Table *t, const Relation *r, bool want_prov, Err *e);
void table_free(Table *t);
/* add a row (dedup): *added tells whether it was new. A table past 2^32-2 rows is datalog-budget. */
Status table_add(Table *t, uint64_t h, const I128 *vals, Origin org, uint32_t ref, uint32_t round, const uint32_t *body, int nbody,
                 bool *added, Err *e);
bool table_contains(const Table *t, const I128 *vals);
bool table_contains_h(const Table *t, uint64_t h, const I128 *vals); /* the same test with the hash given */
uint64_t table_hash_row(const Table *t, const I128 *vals); /* the hash of the dedup set (E5) over all columns */

/* hash of the cells at `cols` (ascending) taken from vals_by_col[column] */
uint64_t hash_cells(const Table *t, const uint32_t *cols, uint32_t nb, const I128 *vals_by_col);
Status table_index(Table *t, const uint32_t *cols, uint32_t nb, Index **out, Err *e);
Status index_sync(Index *ix, const Table *t, Err *e);
/* the bucket of the key taken from vals_by_col (cells by column), or NULL */
const Bucket *index_find(const Index *ix, const Table *t, const I128 *vals_by_col, uint8_t *key); /* key: scratch of kbytes bytes */
static inline const uint32_t *bucket_ids(const Bucket *b) { return (const uint32_t *)b->blk; }
static inline const uint8_t *bucket_cells(const Bucket *b) { return b->blk + (size_t)b->cap * sizeof(uint32_t); }

/* load DIR/<relation>.facts into the table (S82 2) */
Status facts_load(Table *t, const Relation *r, SymTab *syms, const char *path, Err *e);

#endif
