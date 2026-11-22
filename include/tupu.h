/* Public C11 in-process library API for tupu (datalog-engine.md I1). */
#ifndef TUPU_H
#define TUPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TupuSession TupuSession;

/* Error codes matching tupu's closed rejection code list. */
enum {
  TUPU_CODE_USAGE = 0,
  TUPU_CODE_PARSE = 1,
  TUPU_CODE_TYPE_MISMATCH = 2,
  TUPU_CODE_UNRESOLVED_NAME = 3,
  TUPU_CODE_RELATION_ARITY = 4,
  TUPU_CODE_RULE_UNSAFE = 5,
  TUPU_CODE_NEGATION_CYCLE = 6,
  TUPU_CODE_FACTS_MISSING_FILE = 7,
  TUPU_CODE_FACTS_MALFORMED = 8,
  TUPU_CODE_IO_FAILED = 9,
  TUPU_CODE_DATALOG_ARITH = 10,
  TUPU_CODE_DATALOG_BUDGET = 11,
  TUPU_CODE_ENGINE_OUT_OF_MEMORY = 12,
  TUPU_CODE_PROVENANCE_MISMATCH = 13
};

TupuSession *tupu_open(const char *src, size_t len, const char *name, uint64_t budget, size_t nthreads,
                       char *err_buf, size_t err_cap, int *err_code);
void tupu_close(TupuSession *s);

/* Reset all tables, indexes, and counters for the next frame without freeing buffers or restarting the thread pool. */
int tupu_reset(TupuSession *s);

/* Relation metadata. tupu_rel_id returns -1 if not found; tupu_rel_ncols returns -1 on invalid rel_id. */
int tupu_rel_id(const TupuSession *s, const char *name);
int tupu_rel_ncols(const TupuSession *s, int rel_id);

/* Symbol interning and reverse lookup. */
int tupu_intern(TupuSession *s, const char *str, size_t len, uint32_t *out_id);
const char *tupu_sym_str(const TupuSession *s, uint32_t id, size_t *out_len);

/* Insert an input row (BASE_SYMBOL cast to uint32_t, BASE_NUMBER cast to I128). Returns 0 on success, -1 on error. */
int tupu_add_row(TupuSession *s, int rel_id, const int64_t *vals, int ncols);

/* Evaluate the program against currently loaded rows. Returns 0 on success, -1 on rejection/error. */
int tupu_eval(TupuSession *s, int *err_code, char *err_buf, size_t err_cap);

/* Read derived/stored rows. */
uint32_t tupu_rel_count(const TupuSession *s, int rel_id);
int tupu_rel_read_row(const TupuSession *s, int rel_id, uint32_t row, int64_t *out_vals, int ncols);

#ifdef __cplusplus
}
#endif

#endif
