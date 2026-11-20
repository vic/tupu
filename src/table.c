#include "table.h"
#include "num.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Status table_init(Table *t, const Relation *r, bool want_prov, Err *e) {
  memset(t, 0, sizeof *t);
  t->ncols = r->ncols;
  t->kind = malloc(((size_t)r->ncols + 1) * sizeof *t->kind);
  t->col = calloc((size_t)r->ncols + 1, sizeof *t->col);
  if (t->kind == NULL || t->col == NULL) return err_oom(e);
  for (int c = 0; c < r->ncols; c++) t->kind[c] = r->colkind[c];
  t->want_prov = want_prov;
  return STATUS_OK;
}

void table_free(Table *t) {
  for (int c = 0; c < t->ncols && t->col != NULL; c++) free(t->col[c]);
  free(t->col);
  free(t->kind);
  free(t->org_kind);
  free(t->org_ref);
  free(t->round);
  free(t->dslot);
  free(t->dused);
  for (int i = 0; i < t->nidx; i++) {
    Index *ix = t->idx[i];
    free(ix->cols);
    free(ix->hash);
    free(ix->first);
    free(ix->used);
    free(ix->next);
    free(ix);
  }
  free(t->idx);
  free(t->poff);
  free(t->pool);
  memset(t, 0, sizeof *t);
}

static uint64_t fold_cell(uint64_t acc, Base k, I128 v) {
  if (k == BASE_SYMBOL) return mix64(acc ^ (uint64_t)v);
  U128 u = (U128)v;
  acc = mix64(acc ^ (uint64_t)u);
  return mix64(acc ^ (uint64_t)(u >> 64));
}

uint64_t hash_cells(const Table *t, const uint32_t *cols, uint32_t nb, const I128 *vals_by_col) {
  uint64_t acc = 0;
  for (uint32_t i = 0; i < nb; i++) acc = fold_cell(acc, t->kind[cols[i]], vals_by_col[cols[i]]);
  return acc;
}

static uint64_t hash_vals_all(const Table *t, const I128 *vals) {
  uint64_t acc = 0;
  for (int c = 0; c < t->ncols; c++) acc = fold_cell(acc, t->kind[c], vals[c]);
  return acc;
}

static uint64_t hash_row(const Table *t, const uint32_t *cols, uint32_t nb, uint32_t row) {
  uint64_t acc = 0;
  for (uint32_t i = 0; i < nb; i++) acc = fold_cell(acc, t->kind[cols[i]], tcell(t, (int)cols[i], row));
  return acc;
}

static uint64_t hash_row_all(const Table *t, uint32_t row) {
  uint64_t acc = 0;
  for (int c = 0; c < t->ncols; c++) acc = fold_cell(acc, t->kind[c], tcell(t, c, row));
  return acc;
}

static bool row_equals(const Table *t, uint32_t row, const I128 *vals) {
  for (int c = 0; c < t->ncols; c++)
    if (tcell(t, c, row) != vals[c]) return false;
  return true;
}

static Status dedup_grow(Table *t, Err *e) {
  size_t ncap = t->dcap == 0 ? 1024 : t->dcap * 2;
  uint32_t *ns = malloc(ncap * sizeof *ns);
  uint8_t *nu = calloc(ncap, 1);
  if (ns == NULL || nu == NULL) {
    free(ns);
    free(nu);
    return err_oom(e);
  }
  for (uint32_t r = 0; r < t->count; r++) {
    size_t i = hash_row_all(t, r) & (ncap - 1);
    while (nu[i]) i = (i + 1) & (ncap - 1);
    nu[i] = 1;
    ns[i] = r;
  }
  free(t->dslot);
  free(t->dused);
  t->dslot = ns;
  t->dused = nu;
  t->dcap = ncap;
  return STATUS_OK;
}

static Status table_grow(Table *t, Err *e) {
  uint32_t ncap = t->cap == 0 ? 1024 : (t->cap > UINT32_MAX / 2 ? UINT32_MAX : t->cap * 2);
  void **ncol = calloc((size_t)t->ncols + 1, sizeof *ncol);
  if (ncol == NULL) return err_oom(e);
  bool fail = false;
  for (int c = 0; c < t->ncols; c++) {
    size_t w = t->kind[c] == BASE_SYMBOL ? sizeof(uint32_t) : sizeof(I128);
    ncol[c] = malloc((size_t)ncap * w);
    if (ncol[c] == NULL) fail = true;
  }
  uint8_t *ok = malloc(ncap);
  uint32_t *or2 = malloc((size_t)ncap * sizeof *or2), *rd = malloc((size_t)ncap * sizeof *rd);
  size_t *po = t->want_prov ? malloc((size_t)ncap * sizeof *po) : NULL;
  if (ok == NULL || or2 == NULL || rd == NULL || (t->want_prov && po == NULL)) fail = true;
  if (fail) {
    for (int c = 0; c < t->ncols; c++) free(ncol[c]);
    free(ncol);
    free(ok); free(or2); free(rd); free(po);
    return err_oom(e);
  }
  for (int c = 0; c < t->ncols; c++) {
    size_t w = t->kind[c] == BASE_SYMBOL ? sizeof(uint32_t) : sizeof(I128);
    if (t->count > 0) memcpy(ncol[c], t->col[c], (size_t)t->count * w);
    free(t->col[c]);
    t->col[c] = ncol[c];
  }
  if (t->count > 0) {
    memcpy(ok, t->org_kind, t->count);
    memcpy(or2, t->org_ref, (size_t)t->count * sizeof *or2);
    memcpy(rd, t->round, (size_t)t->count * sizeof *rd);
    if (t->want_prov) memcpy(po, t->poff, (size_t)t->count * sizeof *po);
  }
  free(t->org_kind); free(t->org_ref); free(t->round); free(t->poff);
  t->org_kind = ok; t->org_ref = or2; t->round = rd; t->poff = po;
  t->cap = ncap;
  free(ncol);
  return STATUS_OK;
}

uint64_t table_hash_row(const Table *t, const I128 *vals) { return hash_vals_all(t, vals); }

bool table_contains_h(const Table *t, uint64_t h, const I128 *vals) {
  if (t->dcap == 0) return false;
  size_t i = h & (t->dcap - 1);
  while (t->dused[i]) {
    if (row_equals(t, t->dslot[i], vals)) return true;
    i = (i + 1) & (t->dcap - 1);
  }
  return false;
}

bool table_contains(const Table *t, const I128 *vals) {
  if (t->dcap == 0) return false;
  size_t i = hash_vals_all(t, vals) & (t->dcap - 1);
  while (t->dused[i]) {
    if (row_equals(t, t->dslot[i], vals)) return true;
    i = (i + 1) & (t->dcap - 1);
  }
  return false;
}

Status table_add(Table *t, uint64_t h, const I128 *vals, Origin org, uint32_t ref, uint32_t round, const uint32_t *body, int nbody,
                 bool *added, Err *e) {
  *added = false;
  if (t->dcap == 0 || (size_t)(t->count + 1) * 2 > t->dcap) {
    if (dedup_grow(t, e) != STATUS_OK) return STATUS_ERR;
  }
  size_t i = h & (t->dcap - 1);
  while (t->dused[i]) {
    if (row_equals(t, t->dslot[i], vals)) return STATUS_OK;
    i = (i + 1) & (t->dcap - 1);
  }
#ifdef MUTANT_DEDUPREL
  if (t->count > 0) return STATUS_OK;
#endif
  if (t->count == UINT32_MAX - 1) return err_set(e, CODE_DATALOG_BUDGET, NULL, 0, "a table reached 2^32-2 rows");
  if (t->count == t->cap && table_grow(t, e) != STATUS_OK) return STATUS_ERR;
  if (t->want_prov) {
    if (t->pool_n + (size_t)nbody > t->pool_cap) {
      size_t nc = t->pool_cap == 0 ? 4096 : t->pool_cap * 2;
      while (nc < t->pool_n + (size_t)nbody) nc *= 2;
      uint32_t *np = realloc(t->pool, nc * sizeof *np);
      if (np == NULL) return err_oom(e);
      t->pool = np;
      t->pool_cap = nc;
    }
    t->poff[t->count] = t->pool_n;
    for (int b = 0; b < nbody; b++) t->pool[t->pool_n++] = body[b];
  }
  uint32_t row = t->count;
  for (int c = 0; c < t->ncols; c++) {
    if (t->kind[c] == BASE_SYMBOL) ((uint32_t *)t->col[c])[row] = (uint32_t)vals[c];
    else ((I128 *)t->col[c])[row] = vals[c];
  }
  t->org_kind[row] = (uint8_t)org;
  t->org_ref[row] = ref;
  t->round[row] = round;
  t->dused[i] = 1;
  t->dslot[i] = row;
  t->count++;
  *added = true;
  return STATUS_OK;
}

/* ---- indexes ---- */
Status table_index(Table *t, const uint32_t *cols, uint32_t nb, Index **out, Err *e) {
  for (int i = 0; i < t->nidx; i++)
    if (t->idx[i]->nb == nb && memcmp(t->idx[i]->cols, cols, (size_t)nb * sizeof *cols) == 0) {
      *out = t->idx[i];
      return STATUS_OK;
    }
  if (t->nidx == t->idxcap) {
    int nc = t->idxcap == 0 ? 4 : t->idxcap * 2;
    Index **ni = realloc(t->idx, (size_t)nc * sizeof *ni);
    if (ni == NULL) return err_oom(e);
    t->idx = ni;
    t->idxcap = nc;
  }
  Index *ix = calloc(1, sizeof *ix);
  if (ix == NULL) return err_oom(e);
  ix->cols = malloc(((size_t)nb + 1) * sizeof *cols);
  if (ix->cols == NULL) {
    free(ix);
    return err_oom(e);
  }
  memcpy(ix->cols, cols, (size_t)nb * sizeof *cols);
  ix->nb = nb;
  t->idx[t->nidx++] = ix;
  *out = ix;
  return STATUS_OK;
}

static Status slots_grow(Index *ix, const Table *t, Err *e) {
  (void)t;
  size_t ncap = ix->scap == 0 ? 256 : ix->scap * 2;
  uint64_t *nh = malloc(ncap * sizeof *nh);
  uint32_t *nf = malloc(ncap * sizeof *nf);
  uint8_t *nu = calloc(ncap, 1);
  if (nh == NULL || nf == NULL || nu == NULL) {
    free(nh); free(nf); free(nu);
    return err_oom(e);
  }
  for (size_t i = 0; i < ix->scap; i++) {
    if (!ix->used[i]) continue;
    size_t j = ix->hash[i] & (ncap - 1);
    while (nu[j]) j = (j + 1) & (ncap - 1);
    nu[j] = 1;
    nh[j] = ix->hash[i];
    nf[j] = ix->first[i];
  }
  free(ix->hash); free(ix->first); free(ix->used);
  ix->hash = nh; ix->first = nf; ix->used = nu; ix->scap = ncap;
  return STATUS_OK;
}

Status index_sync(Index *ix, const Table *t, Err *e) {
  if (ix->synced == t->count) return STATUS_OK;
  if (ix->next_cap < t->count) {
    size_t nc = ix->next_cap == 0 ? 1024 : ix->next_cap;
    while (nc < t->count) nc *= 2;
    uint32_t *nn = realloc(ix->next, nc * sizeof *nn);
    if (nn == NULL) return err_oom(e);
    ix->next = nn;
    ix->next_cap = nc;
  }
  for (uint32_t r = ix->synced; r < t->count; r++) {
    if (ix->scap == 0 || (ix->nslots + 1) * 2 > ix->scap) {
      if (slots_grow(ix, t, e) != STATUS_OK) return STATUS_ERR;
    }
    uint64_t h = hash_row(t, ix->cols, ix->nb, r);
    size_t i = h & (ix->scap - 1);
    while (ix->used[i] && ix->hash[i] != h) i = (i + 1) & (ix->scap - 1);
    if (ix->used[i]) {
      ix->next[r] = ix->first[i];
      ix->first[i] = r;
    } else {
      ix->used[i] = 1;
      ix->hash[i] = h;
      ix->first[i] = r;
      ix->next[r] = r;
      ix->nslots++;
    }
    ix->synced = r + 1;
  }
  return STATUS_OK;
}

bool index_find(const Index *ix, uint64_t hash, uint32_t *row) {
  if (ix->scap == 0) return false;
  size_t i = hash & (ix->scap - 1);
  while (ix->used[i]) {
    if (ix->hash[i] == hash) {
      *row = ix->first[i];
      return true;
    }
    i = (i + 1) & (ix->scap - 1);
  }
  return false;
}

/* ---- facts files ---- */
Status facts_load(Table *t, const Relation *r, SymTab *syms, const char *path, Err *e) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return err_set(e, CODE_FACTS_MISSING_FILE, path, 0, "cannot open the facts file of %.*s", (int)r->name.n, r->name.s);
  size_t cap = 1 << 16, len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    (void)fclose(f);
    return err_oom(e);
  }
  for (;;) {
    if (len == cap) {
      char *nb = realloc(buf, cap * 2);
      if (nb == NULL) {
        free(buf);
        (void)fclose(f);
        return err_oom(e);
      }
      buf = nb;
      cap *= 2;
    }
    size_t got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0) break;
  }
  bool bad = ferror(f) != 0;
  (void)fclose(f);
  if (bad) {
    free(buf);
    return err_set(e, CODE_IO_FAILED, path, 0, "read failed");
  }
  I128 *vals = malloc(((size_t)t->ncols + 1) * sizeof *vals);
  if (vals == NULL) {
    free(buf);
    return err_oom(e);
  }
  long line = 0;
  size_t p = 0;
  Status st = STATUS_OK;
  while (p < len && st == STATUS_OK) {
    size_t eol = p;
    while (eol < len && buf[eol] != '\n') eol++;
    line++;
    size_t q = p;
    int c = 0;
    bool malformed = false;
    if (t->ncols == 0) {
      malformed = eol != p;
    } else {
      for (;;) {
        size_t tab = q;
        while (tab < eol && buf[tab] != '\t') tab++;
        if (c >= t->ncols) {
          malformed = true;
          break;
        }
        if (t->kind[c] == BASE_SYMBOL) {
          uint32_t id;
          if (sym_intern(syms, buf + q, tab - q, &id, e) != STATUS_OK) {
            st = STATUS_ERR;
            break;
          }
          vals[c] = (I128)id;
        } else if (!num_parse(buf + q, tab - q, &vals[c])) {
          malformed = true;
          break;
        }
        c++;
        if (tab >= eol) break;
        q = tab + 1;
      }
      if (st == STATUS_OK && !malformed && c != t->ncols) malformed = true;
    }
    if (st == STATUS_OK && malformed) st = err_set(e, CODE_FACTS_MALFORMED, path, line, "a row of %.*s is malformed", (int)r->name.n, r->name.s);
    if (st == STATUS_OK) {
      bool added;
      st = table_add(t, table_hash_row(t, vals), vals, ORG_INPUT, 0, 0, NULL, 0, &added, e);
    }
    p = eol + 1;
  }
  free(buf);
  free(vals);
  return st;
}
