/* Checks after reading: names, arity, types (S77, S81 3), stratification (E2). Safety is checked by the planner. */
#ifndef TUPU_CHECK_H
#define TUPU_CHECK_H
#include "ast.h"

typedef struct {
  int ncomps;
  /* component of each relation, numbered in the order of the sequential stratification (E2): the order in
     which iterative Tarjan emits the components of the dependency graph built in declaration order */
  int *comp;
  /* per relation: true when a rule of its component reads a relation of the same component (recursive) */
  bool *recursive;
} Strata;

Status check_program(Program *p, Err *e);
Status stratify(Program *p, Strata *s, Err *e);
void strata_free(Strata *s);

/* type sets: intersection and emptiness, for the planner and the checker */
bool ts_empty(const Program *p, const TypeSet *t);

#endif
