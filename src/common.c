#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *code_name(Code c) {
  switch (c) {
  case CODE_USAGE: return "usage";
  case CODE_PARSE: return "parse";
  case CODE_TYPE_MISMATCH: return "type-mismatch";
  case CODE_UNRESOLVED_NAME: return "unresolved-name";
  case CODE_RELATION_ARITY: return "relation-arity";
  case CODE_RULE_UNSAFE: return "rule-unsafe";
  case CODE_NEGATION_CYCLE: return "negation-cycle";
  case CODE_FACTS_MISSING_FILE: return "facts-missing-file";
  case CODE_FACTS_MALFORMED: return "facts-malformed";
  case CODE_IO_FAILED: return "io-failed";
  case CODE_DATALOG_ARITH: return "datalog-arith";
  case CODE_DATALOG_BUDGET: return "datalog-budget";
  case CODE_ENGINE_OUT_OF_MEMORY: return "engine-out-of-memory";
  case CODE_PROVENANCE_MISMATCH: return "provenance-mismatch";
  }
  return "usage"; /* unreachable: the switch covers the closed enum */
}

Status err_set(Err *e, Code c, const char *file, long line, const char *fmt, ...) {
  va_list ap;
  e->code = c;
  e->file = file;
  e->line = line;
  va_start(ap, fmt);
  (void)vsnprintf(e->text, sizeof e->text, fmt, ap);
  va_end(ap);
  return STATUS_ERR;
}

Status err_oom(Err *e) { return err_set(e, CODE_ENGINE_OUT_OF_MEMORY, NULL, 0, "allocation failed"); }

struct ABlock {
  ABlock *next;
  size_t used, cap;
  _Alignas(16) unsigned char data[];
};

void *arena_alloc(Arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (a->head == NULL || a->head->cap - a->head->used < n) {
    size_t cap = n > 65536 ? n : 65536;
    ABlock *b = malloc(sizeof *b + cap);
    if (b == NULL) return NULL;
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += n;
  return p;
}

void arena_free(Arena *a) {
  ABlock *b = a->head;
  while (b != NULL) {
    ABlock *n = b->next;
    free(b);
    b = n;
  }
  a->head = NULL;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
  char *p = arena_alloc(a, n + 1);
  if (p == NULL) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

Status arr_reserve(Arena *a, void **p, size_t *cap, size_t need, size_t elem, Err *e) {
  if (need <= *cap) return STATUS_OK;
  size_t ncap = *cap == 0 ? 8 : *cap * 2;
  while (ncap < need) ncap *= 2;
  void *np = arena_alloc(a, ncap * elem);
  if (np == NULL) return err_oom(e);
  if (*p != NULL) memcpy(np, *p, *cap * elem);
  *p = np;
  *cap = ncap;
  return STATUS_OK;
}
