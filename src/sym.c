#include "sym.h"
#include <stdlib.h>
#include <string.h>

static uint64_t hash_bytes(const char *p, size_t n) {
  uint64_t h = 0;
  for (size_t i = 0; i < n; i++) h = mix64(h ^ (uint64_t)(unsigned char)p[i]);
  return mix64(h ^ (uint64_t)n);
}

void sym_init(SymTab *s) { memset(s, 0, sizeof *s); }

void sym_free(SymTab *s) {
  free(s->str);
  free(s->len);
  free(s->slot);
  free(s->used);
  free(s->rank);
  arena_free(&s->bytes);
  memset(s, 0, sizeof *s);
}

static Status grow_slots(SymTab *s, Err *e) {
  size_t ncap = s->scap == 0 ? 1024 : s->scap * 2;
  uint32_t *ns = malloc(ncap * sizeof *ns);
  uint8_t *nu = calloc(ncap, 1);
  if (ns == NULL || nu == NULL) {
    free(ns);
    free(nu);
    return err_oom(e);
  }
  for (uint32_t id = 0; id < s->n; id++) {
    size_t i = hash_bytes(s->str[id], s->len[id]) & (ncap - 1);
    while (nu[i]) i = (i + 1) & (ncap - 1);
    nu[i] = 1;
    ns[i] = id;
  }
  free(s->slot);
  free(s->used);
  s->slot = ns;
  s->used = nu;
  s->scap = ncap;
  return STATUS_OK;
}

Status sym_intern(SymTab *s, const char *p, size_t n, uint32_t *id, Err *e) {
  if (s->scap == 0 || (size_t)s->n * 2 >= s->scap) {
    if (grow_slots(s, e) != STATUS_OK) return STATUS_ERR;
  }
  size_t i = hash_bytes(p, n) & (s->scap - 1);
  while (s->used[i]) {
    uint32_t c = s->slot[i];
    if (s->len[c] == n && memcmp(s->str[c], p, n) == 0) {
      *id = c;
      return STATUS_OK;
    }
    i = (i + 1) & (s->scap - 1);
  }
  if (s->n == s->cap) {
    uint32_t ncap = s->cap == 0 ? 1024 : s->cap * 2;
    const char **nstr = realloc((void *)s->str, ncap * sizeof *nstr);
    if (nstr == NULL) return err_oom(e);
    s->str = nstr;
    uint32_t *nlen = realloc(s->len, ncap * sizeof *nlen);
    if (nlen == NULL) return err_oom(e);
    s->len = nlen;
    s->cap = ncap;
  }
  char *copy = arena_strndup(&s->bytes, p, n);
  if (copy == NULL) return err_oom(e);
  s->str[s->n] = copy;
  s->len[s->n] = (uint32_t)n;
  s->used[i] = 1;
  s->slot[i] = s->n;
  *id = s->n++;
  return STATUS_OK;
}

static _Thread_local const SymTab *cmp_tab;
static int cmp_ids(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  size_t lx = cmp_tab->len[x], ly = cmp_tab->len[y];
  int c = memcmp(cmp_tab->str[x], cmp_tab->str[y], lx < ly ? lx : ly);
  if (c != 0) return c;
  return (lx > ly) - (lx < ly);
}

Status sym_build_rank(SymTab *s, Err *e) {
  free(s->rank);
  s->rank = NULL;
  if (s->n == 0) return STATUS_OK;
  uint32_t *order = malloc((size_t)s->n * sizeof *order);
  s->rank = malloc((size_t)s->n * sizeof *s->rank);
  if (order == NULL || s->rank == NULL) {
    free(order);
    return err_oom(e);
  }
  for (uint32_t i = 0; i < s->n; i++) order[i] = i;
  cmp_tab = s;
  qsort(order, s->n, sizeof *order, cmp_ids);
  for (uint32_t r = 0; r < s->n; r++) s->rank[order[r]] = r;
  free(order);
  return STATUS_OK;
}
