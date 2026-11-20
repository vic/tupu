#ifndef TUPU_OUT_H
#define TUPU_OUT_H
#include "engine.h"

/* -D: "-" writes every .output relation to stdout (`relation NAME`, then sorted rows); a directory gets NAME.csv */
Status out_write(const Engine *x, const char *target, Err *e);
/* -P: DIR/NAME.prov per .output relation (S82 11) */
Status out_write_prov(const Engine *x, const char *dir, Err *e);

#endif
