/* Shared types: closed status and code enums, the error value, the arena, exact-integer typedefs. */
#ifndef TUPU_COMMON_H
#define TUPU_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__extension__ typedef __int128 I128;
__extension__ typedef unsigned __int128 U128;

typedef enum { STATUS_OK, STATUS_ERR } Status;

/* the closed list of rejection codes (rulings S79, S82) */
typedef enum {
  CODE_USAGE,
  CODE_PARSE,
  CODE_TYPE_MISMATCH,
  CODE_UNRESOLVED_NAME,
  CODE_RELATION_ARITY,
  CODE_RULE_UNSAFE,
  CODE_NEGATION_CYCLE,
  CODE_FACTS_MISSING_FILE,
  CODE_FACTS_MALFORMED,
  CODE_IO_FAILED,
  CODE_DATALOG_ARITH,
  CODE_DATALOG_BUDGET,
  CODE_ENGINE_OUT_OF_MEMORY,
  CODE_PROVENANCE_MISMATCH
} Code;

typedef struct {
  Code code;
  const char *file; /* NULL: a code of the whole run; else points into filebuf */
  long line;
  char filebuf[512];
  char text[256];
} Err;

const char *code_name(Code c);
Status err_set(Err *e, Code c, const char *file, long line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
Status err_oom(Err *e);

/* bump arena: every block is freed together; NULL on allocation failure */
typedef struct ABlock ABlock;
typedef struct {
  ABlock *head;
} Arena;
void *arena_alloc(Arena *a, size_t n);
void arena_free(Arena *a);
char *arena_strndup(Arena *a, const char *s, size_t n);
/* grow a typed array held in the arena; *cap is in elements */
Status arr_reserve(Arena *a, void **p, size_t *cap, size_t need, size_t elem, Err *e);
#define ARR_SLOT(A, ARR, E)                                                                       \
  (arr_reserve((A), (void **)&(ARR).p, &(ARR).cap, (ARR).n + 1, sizeof *(ARR).p, (E)) == STATUS_OK \
       ? &(ARR).p[(ARR).n++]                                                                      \
       : NULL)

/* the splitmix64 finalizer of vic/spec/emitter.md 4 */
static inline uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

#endif
