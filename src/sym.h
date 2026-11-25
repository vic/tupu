/* Symbol interning: each distinct byte string gets a 32-bit id in order of first appearance.
   rank[] (byte order of all symbols, from 0) is built once the domain is fixed (ruling S81 6). */
#ifndef TUPU_SYM_H
#define TUPU_SYM_H
#include "common.h"

typedef struct {
  const char **str; /* id -> bytes (not terminated) */
  uint32_t *len;
  uint32_t n, cap;
  uint32_t *slot; /* open addressing, slot holds id+1 through a parallel used byte */
  uint8_t *used;
  size_t scap;
  uint32_t *rank;
  Arena bytes;
  uint32_t base;  /* ids below base belong to the program (sym_seal); ids from base on to one evaluation */
  bool sealed;
  Arena evbytes;  /* bytes of the evaluation symbols, freed by sym_forget_evaluation */
} SymTab;

void sym_init(SymTab *s);
void sym_free(SymTab *s);
Status sym_intern(SymTab *s, const char *p, size_t n, uint32_t *id, Err *e);
Status sym_build_rank(SymTab *s, Err *e);
/* every symbol interned so far belongs to the program and stays; later ones belong to the current evaluation */
void sym_seal(SymTab *s);
/* forget the symbols of the evaluation: their ids are free again and their bytes are released */
void sym_forget_evaluation(SymTab *s);

#endif
