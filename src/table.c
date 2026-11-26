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
  t->fast1 = (r->ncols == 1 && r->colkind[0] == BASE_SYMBOL);
  t->fast2 = (r->ncols == 2 && r->colkind[0] == BASE_SYMBOL && r->colkind[1] == BASE_SYMBOL);
  t->lat_col = r->lat_col;
  t->lat_max = r->lat_max;
  if (t->lat_col >= 0) {
    t->lkcols = malloc(((size_t)r->ncols + 1) * sizeof *t->lkcols);
    if (t->lkcols == NULL) return err_oom(e);
    for (int c = 0; c < r->ncols; c++)
      if (c != t->lat_col) t->lkcols[t->nlk++] = (uint32_t)c;
  }
  return STATUS_OK;
}

static Status dedup_grow(Table *t, Err *e);

Status table_enable_bitset(Table *t, uint32_t nsyms, Err *e) {
  if (!t->fast2 || nsyms == 0) return STATUS_OK;
  if (t->bits != NULL) {
    if (nsyms <= t->bit_nsyms) return STATUS_OK;
    for (uint32_t i = 0; i < t->bit_nsyms; i++) free(t->bits[i]);
    free(t->bits);
    t->bits = NULL;
    t->bit_nsyms = 0;
    t->bit_words = 0;
    if (nsyms > 8192) {
      while (t->dcap == 0 || (size_t)(t->count + 1) * 20 > t->dcap * 13) {
        if (dedup_grow(t, e) != STATUS_OK) return STATUS_ERR;
      }
      const uint32_t *c0 = (const uint32_t *)t->col[0];
      const uint32_t *c1 = (const uint32_t *)t->col[1];
      for (uint32_t r = 0; r < t->count; r++) {
        uint64_t k = ((uint64_t)c0[r] << 32) | (uint64_t)c1[r];
        size_t i = mix64(k) & (t->dcap - 1);
        while (t->dkey[i] != UINT64_MAX) i = (i + 1) & (t->dcap - 1);
        t->dkey[i] = k;
      }
      return STATUS_OK;
    }
  }
  if (nsyms > 8192) return STATUS_OK;
  uint32_t words = (nsyms + 63u) / 64u;
  uint64_t **bits = calloc(nsyms, sizeof *bits);
  if (bits == NULL) return err_oom(e);
  const uint32_t *c0 = (const uint32_t *)t->col[0];
  const uint32_t *c1 = (const uint32_t *)t->col[1];
  for (uint32_t r = 0; r < t->count; r++) {
    uint32_t v0 = c0[r], v1 = c1[r];
    if (bits[v0] == NULL) {
      bits[v0] = calloc(words, sizeof(uint64_t));
      if (bits[v0] == NULL) {
        for (uint32_t i = 0; i < nsyms; i++) free(bits[i]);
        free(bits);
        return err_oom(e);
      }
    }
    bits[v0][v1 >> 6] |= (1ULL << (v1 & 63));
  }
  t->bits = bits;
  t->bit_nsyms = nsyms;
  t->bit_words = words;
  free(t->dkey);
  t->dkey = NULL;
  t->dcap = 0;
  return STATUS_OK;
}

/* The cost of a clear follows the rows the table held, not the capacity it grew to: every set bit of the dedup bit
   matrix is a row of the table (table_add sets it, table_enable_bitset rebuilds the bits from the rows), and every entry
   of a direct index is the symbol of a synced row; so those are cleared row by row (X28-bb: whole-matrix memsets were
   40 GB per load of the stable tree). */
void table_clear(Table *t) {
  if (t->count == 0) return;
  if (t->bits != NULL && t->count > 0) {
    const uint32_t *c0 = (const uint32_t *)t->col[0];
    const uint32_t *c1 = (const uint32_t *)t->col[1];
#ifdef MUTANT_CLEARROWS
    for (uint32_t r = 1; r < t->count; r++) t->bits[c0[r]][c1[r] >> 6] = 0; /* control: one row's bit stays */
#else
    for (uint32_t r = 0; r < t->count; r++) t->bits[c0[r]][c1[r] >> 6] = 0;
#endif
  }
  for (int i = 0; i < t->nidx; i++) {
    Index *ix = t->idx[i];
    if (ix->direct == NULL || ix->synced == 0) continue;
    const uint32_t *colp = (const uint32_t *)t->col[ix->cols[0]];
    for (uint32_t r = 0; r < ix->synced; r++) ix->direct[colp[r]] = UINT32_MAX;
  }
  t->count = 0;
  t->pool_n = 0;
  if (t->dkey != NULL && t->dcap > 0) memset(t->dkey, 0xFF, t->dcap * sizeof *t->dkey);
  if (t->dsl != NULL && t->dcap > 0) memset(t->dsl, 0, t->dcap * sizeof *t->dsl);
  for (int i = 0; i < t->nidx; i++) {
    Index *ix = t->idx[i];
    for (uint32_t b = 0; b < ix->nbk; b++) ix->bks[b].n = 0;
    ix->nbk = 0;
    ix->synced = 0;
    if (ix->used != NULL && ix->scap > 0) memset(ix->used, 0, ix->scap);
  }
  if (t->lbest != NULL && t->lcap > 0) memset(t->lbest, 0, t->lcap * sizeof *t->lbest);
  t->lcount = 0;
}

void table_free(Table *t) {
  for (int c = 0; c < t->ncols && t->col != NULL; c++) free(t->col[c]);
  free(t->lkcols);
  free(t->lbest);
  free(t->col);
  free(t->kind);
  free(t->org_kind);
  free(t->org_ref);
  free(t->round);
  free(t->dsl);
  free(t->dkey);
  if (t->bits != NULL) {
    for (uint32_t i = 0; i < t->bit_nsyms; i++) free(t->bits[i]);
    free(t->bits);
  }
  for (int i = 0; i < t->nidx; i++) {
    Index *ix = t->idx[i];
    free(ix->cols);
    free(ix->hash);
    for (uint32_t b = 0; b < ix->bkcap; b++) free(ix->bks[b].blk);
    free(ix->bks);
    free(ix->keys);
    free(ix->fcols);
    free(ix->bk);
    free(ix->used);
    free(ix->direct);
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

static inline uint64_t pack_fast_key(const Table *t, const I128 *vals) {
  return t->fast2 ? (((uint64_t)(uint32_t)vals[0] << 32) | (uint64_t)(uint32_t)vals[1]) : (uint64_t)(uint32_t)vals[0];
}

static Status dedup_grow(Table *t, Err *e) {
  size_t ncap = t->dcap == 0 ? 1024 : t->dcap * 2;
  if (t->fast1 || t->fast2) {
    uint64_t *nk = malloc(ncap * sizeof *nk);
    if (nk == NULL) return err_oom(e);
    memset(nk, 0xFF, ncap * sizeof *nk);
    for (size_t j = 0; j < t->dcap; j++) {
      uint64_t k = t->dkey[j];
      if (k == UINT64_MAX) continue;
      size_t i = mix64(k) & (ncap - 1);
      while (nk[i] != UINT64_MAX) i = (i + 1) & (ncap - 1);
      nk[i] = k;
    }
    free(t->dkey);
    t->dkey = nk;
    t->dcap = ncap;
    return STATUS_OK;
  }
  DSlot *ns = calloc(ncap, sizeof *ns);
  if (ns == NULL) return err_oom(e);
  for (uint32_t r = 0; r < t->count; r++) { /* growth only: each row is hashed again from its columns */
    uint64_t h = hash_row_all(t, r);
    size_t i = h & (ncap - 1);
    while (ns[i].used) i = (i + 1) & (ncap - 1);
    ns[i].used = 1;
    ns[i].tag = hash_tag(h);
    ns[i].row = r;
  }
  free(t->dsl);
  t->dsl = ns;
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
  uint8_t *ok = t->want_prov ? malloc(ncap) : NULL;
  uint32_t *or2 = t->want_prov ? malloc((size_t)ncap * sizeof *or2) : NULL;
  uint32_t *rd = t->want_prov ? malloc((size_t)ncap * sizeof *rd) : NULL;
  size_t *po = t->want_prov ? malloc((size_t)ncap * sizeof *po) : NULL;
  if (t->want_prov && (ok == NULL || or2 == NULL || rd == NULL || po == NULL)) fail = true;
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
  if (t->want_prov) {
    if (t->count > 0) {
      memcpy(ok, t->org_kind, t->count);
      memcpy(or2, t->org_ref, (size_t)t->count * sizeof *or2);
      memcpy(rd, t->round, (size_t)t->count * sizeof *rd);
      memcpy(po, t->poff, (size_t)t->count * sizeof *po);
    }
    free(t->org_kind); free(t->org_ref); free(t->round); free(t->poff);
    t->org_kind = ok; t->org_ref = or2; t->round = rd; t->poff = po;
  }
  t->cap = ncap;
  free(ncol);
  return STATUS_OK;
}

uint64_t table_hash_row(const Table *t, const I128 *vals) {
  if (t->fast1 || t->fast2) return mix64(pack_fast_key(t, vals));
  return hash_vals_all(t, vals);
}

bool table_contains_h(const Table *t, uint64_t h, const I128 *vals) {
  if (t->bits != NULL) {
    uint32_t v0 = (uint32_t)vals[0], v1 = (uint32_t)vals[1];
    return v0 < t->bit_nsyms && v1 < t->bit_nsyms && t->bits[v0] != NULL && ((t->bits[v0][v1 >> 6] >> (v1 & 63)) & 1ULL) != 0;
  }
  if (t->dcap == 0) return false;
  if (t->fast1 || t->fast2) return table_contains_fast2(t, pack_fast_key(t, vals), h);
  size_t i = h & (t->dcap - 1);
  while (t->dsl[i].used) {
    if (t->dsl[i].tag == hash_tag(h) && row_equals(t, t->dsl[i].row, vals)) return true; /* equal hashes: read the columns; never trust a hash alone */
    i = (i + 1) & (t->dcap - 1);
  }
  return false;
}

bool table_contains(const Table *t, const I128 *vals) {
  uint64_t h = table_hash_row(t, vals);
  return table_contains_h(t, h, vals);
}

static bool lattice_key_equals(const Table *t, uint32_t row, const I128 *vals) {
  for (uint32_t k = 0; k < t->nlk; k++)
    if (tcell(t, (int)t->lkcols[k], row) != vals[t->lkcols[k]]) return false;
  return true;
}

static Status lattice_grow(Table *t, Err *e) {
  size_t ncap = t->lcap == 0 ? 64 : t->lcap * 2;
  uint32_t *nb = calloc(ncap, sizeof *nb);
  I128 *vals = malloc(((size_t)t->ncols + 1) * sizeof *vals);
  if (nb == NULL || vals == NULL) {
    free(nb);
    free(vals);
    return err_oom(e);
  }
  for (size_t s = 0; s < t->lcap; s++) {
    if (t->lbest[s] == 0) continue;
    uint32_t row = t->lbest[s] - 1;
    for (uint32_t k = 0; k < t->nlk; k++) vals[t->lkcols[k]] = tcell(t, (int)t->lkcols[k], row);
    size_t j = hash_cells(t, t->lkcols, t->nlk, vals) & (ncap - 1);
    while (nb[j] != 0) j = (j + 1) & (ncap - 1);
    nb[j] = t->lbest[s];
  }
  free(vals);
  free(t->lbest);
  t->lbest = nb;
  t->lcap = ncap;
  return STATUS_OK;
}

Status table_add(Table *t, uint64_t h, const I128 *vals, Origin org, uint32_t ref, uint32_t round, const uint32_t *body, int nbody,
                 bool *added, Err *e) {
  *added = false;
  size_t li = 0;
  bool lnew = false;
  if (t->lat_col >= 0) { /* subsumption: a row that does not improve the best of its key is not added */
    if ((size_t)(t->lcount + 1) * 2 > t->lcap && lattice_grow(t, e) != STATUS_OK) return STATUS_ERR;
    li = hash_cells(t, t->lkcols, t->nlk, vals) & (t->lcap - 1);
    lnew = true;
    while (t->lbest[li] != 0) {
      uint32_t b = t->lbest[li] - 1;
      if (lattice_key_equals(t, b, vals)) {
        I128 old = tcell(t, t->lat_col, b), now = vals[t->lat_col];
#ifdef MUTANT_LATTICEDIR
        if (!(t->lat_max ? now < old : now > old)) return STATUS_OK;
#else
        if (!(t->lat_max ? now > old : now < old)) return STATUS_OK;
#endif
        lnew = false;
        break;
      }
      li = (li + 1) & (t->lcap - 1);
    }
  }
  size_t i = 0;
  uint64_t fkey = 0;
  uint32_t bv0 = 0, bv1 = 0;
  if (t->bits != NULL) {
    bv0 = (uint32_t)vals[0];
    bv1 = (uint32_t)vals[1];
    if (bv0 >= t->bit_nsyms || bv1 >= t->bit_nsyms) {
      uint32_t need = (bv0 > bv1 ? bv0 : bv1) + 1;
      uint32_t target = need <= 8192 ? (need < 256 ? 256 : (need * 2 <= 8192 ? need * 2 : 8192)) : need;
      if (table_enable_bitset(t, target, e) != STATUS_OK) return STATUS_ERR;
    }
  }
  if (t->bits != NULL) {
    if (t->bits[bv0] != NULL && ((t->bits[bv0][bv1 >> 6] >> (bv1 & 63)) & 1ULL) != 0) return STATUS_OK;
  } else {
    bool need_grow = t->dcap == 0 || ((t->fast1 || t->fast2) ? ((size_t)(t->count + 1) * 20 > t->dcap * 13) : ((size_t)(t->count + 1) * 2 > t->dcap));
    if (need_grow) {
      if (dedup_grow(t, e) != STATUS_OK) return STATUS_ERR;
    }
    i = h & (t->dcap - 1);
    if (t->fast1 || t->fast2) {
      fkey = pack_fast_key(t, vals);
      while (t->dkey[i] != UINT64_MAX) {
        if (t->dkey[i] == fkey) return STATUS_OK;
        i = (i + 1) & (t->dcap - 1);
      }
    } else {
      while (t->dsl[i].used) {
        if (t->dsl[i].tag == hash_tag(h) && row_equals(t, t->dsl[i].row, vals)) return STATUS_OK;
        i = (i + 1) & (t->dcap - 1);
      }
    }
  }
#ifdef MUTANT_DEDUPREL
  if (t->count > 0) return STATUS_OK;
#endif
  if (t->count == UINT32_MAX - 1) return err_set(e, CODE_DATALOG_BUDGET, NULL, 0, "a table reached 2^32-2 rows");
  if (t->bits != NULL && t->bits[bv0] == NULL) {
    t->bits[bv0] = calloc(t->bit_words, sizeof(uint64_t));
    if (t->bits[bv0] == NULL) return err_oom(e);
  }
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
  if (t->want_prov) {
    t->org_kind[row] = (uint8_t)org;
    t->org_ref[row] = ref;
    t->round[row] = round;
  }
  if (t->bits != NULL) {
    t->bits[bv0][bv1 >> 6] |= (1ULL << (bv1 & 63));
  } else if (t->fast1 || t->fast2) {
    t->dkey[i] = fkey;
  } else {
    t->dsl[i].used = 1;
    t->dsl[i].tag = hash_tag(h);
    t->dsl[i].row = row;
  }
  if (t->lat_col >= 0) {
    t->lbest[li] = row + 1;
    if (lnew) t->lcount++;
  }
  t->count++;
  *added = true;
  return STATUS_OK;
}

/* ---- covering indexes (S102) ---- */
static size_t cell_bytes(const Table *t, uint32_t c) { return t->kind[c] == BASE_SYMBOL ? sizeof(uint32_t) : sizeof(I128); }

static void put_cell(const Table *t, uint32_t c, uint8_t *dst, I128 v) {
  if (t->kind[c] == BASE_SYMBOL) {
    uint32_t s = (uint32_t)v;
    memcpy(dst, &s, sizeof s);
  } else memcpy(dst, &v, sizeof v);
}

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
  ix->fcols = malloc(((size_t)t->ncols + 1) * sizeof *cols);
  if (ix->cols == NULL || ix->fcols == NULL) {
    free(ix->cols);
    free(ix->fcols);
    free(ix);
    return err_oom(e);
  }
  memcpy(ix->cols, cols, (size_t)nb * sizeof *cols);
  ix->nb = nb;
  ix->direct_sym = (nb == 1 && t->kind[cols[0]] == BASE_SYMBOL);
  for (uint32_t c = 0; c < (uint32_t)t->ncols; c++) {
    bool bound = false;
    for (uint32_t i = 0; i < nb; i++) bound = bound || cols[i] == c;
    if (bound) ix->kbytes += cell_bytes(t, c);
    else {
      ix->fcols[ix->nf++] = c;
      ix->fbytes += cell_bytes(t, c);
    }
  }
  t->idx[t->nidx++] = ix;
  *out = ix;
  return STATUS_OK;
}

static void pack_key(const Table *t, const Index *ix, const I128 *vals_by_col, uint8_t *dst) {
  for (uint32_t i = 0; i < ix->nb; i++) {
    put_cell(t, ix->cols[i], dst, vals_by_col[ix->cols[i]]);
    dst += cell_bytes(t, ix->cols[i]);
  }
}

static Status slots_grow(Index *ix, Err *e) {
  size_t ncap = ix->scap == 0 ? 256 : ix->scap * 2;
  uint64_t *nh = malloc(ncap * sizeof *nh);
  uint32_t *nk = malloc(ncap * sizeof *nk);
  uint8_t *nu = calloc(ncap, 1);
  if (nh == NULL || nk == NULL || nu == NULL) {
    free(nh); free(nk); free(nu);
    return err_oom(e);
  }
  for (size_t i = 0; i < ix->scap; i++) {
    if (!ix->used[i]) continue;
    size_t j = ix->hash[i] & (ncap - 1);
    while (nu[j]) j = (j + 1) & (ncap - 1);
    nu[j] = 1;
    nh[j] = ix->hash[i];
    nk[j] = ix->bk[i];
  }
  free(ix->hash); free(ix->bk); free(ix->used);
  ix->hash = nh; ix->bk = nk; ix->used = nu; ix->scap = ncap;
  return STATUS_OK;
}

/* append (row id, free cells) to a bucket; growth doubles: new block, copy, free the old one (E3) */
static Status bucket_push(const Index *ix, Bucket *b, uint32_t row, const Table *t, Err *e) {
  if (b->n == b->cap) {
    uint32_t nc = b->cap == 0 ? 4 : b->cap * 2;
    uint8_t *nb = malloc((size_t)nc * (sizeof(uint32_t) + ix->fbytes) + 1);
    if (nb == NULL) return err_oom(e);
    if (b->n > 0) {
      memcpy(nb, b->blk, (size_t)b->n * sizeof(uint32_t));
      memcpy(nb + (size_t)nc * sizeof(uint32_t), b->blk + (size_t)b->cap * sizeof(uint32_t), (size_t)b->n * ix->fbytes);
    }
    free(b->blk);
    b->blk = nb;
    b->cap = nc;
  }
  memcpy(b->blk + (size_t)b->n * sizeof(uint32_t), &row, sizeof row);
  uint8_t *cells = b->blk + (size_t)b->cap * sizeof(uint32_t) + (size_t)b->n * ix->fbytes;
  for (uint32_t i = 0; i < ix->nf; i++) {
    put_cell(t, ix->fcols[i], cells, tcell(t, (int)ix->fcols[i], row));
    cells += cell_bytes(t, ix->fcols[i]);
  }
  b->n++;
  return STATUS_OK;
}

Status index_sync(Index *ix, const Table *t, Err *e) {
  if (ix->synced == t->count) return STATUS_OK;
  if (ix->direct_sym) {
    uint32_t col0 = ix->cols[0];
    const uint32_t *colp = (const uint32_t *)t->col[col0];
    Status st = STATUS_OK;
    for (uint32_t r = ix->synced; r < t->count && st == STATUS_OK; r++) {
      uint32_t sym = colp[r];
      if ((size_t)sym >= ix->dircap) {
        size_t nc = ix->dircap == 0 ? 256 : ix->dircap * 2;
        while (nc <= (size_t)sym) nc *= 2;
        uint32_t *nd = realloc(ix->direct, nc * sizeof *nd);
        if (nd == NULL) { st = err_oom(e); break; }
        memset(nd + ix->dircap, 0xFF, (nc - ix->dircap) * sizeof *nd);
        ix->direct = nd;
        ix->dircap = nc;
      }
      uint32_t bi = ix->direct[sym];
      if (bi == UINT32_MAX) {
        if (ix->nbk == ix->bkcap) {
          uint32_t nc = ix->bkcap == 0 ? 256 : ix->bkcap * 2;
          Bucket *nbk = realloc(ix->bks, (size_t)nc * sizeof *nbk);
          if (nbk == NULL) { st = err_oom(e); break; }
          memset(nbk + ix->bkcap, 0, (size_t)(nc - ix->bkcap) * sizeof *nbk);
          ix->bks = nbk;
          ix->bkcap = nc;
        }
        bi = ix->nbk++;
        ix->bks[bi].n = 0;
        ix->direct[sym] = bi;
      }
      st = bucket_push(ix, &ix->bks[bi], r, t, e);
      ix->synced = r + 1;
    }
    return st;
  }
  I128 *vals = malloc(((size_t)t->ncols + 1) * sizeof *vals);
  uint8_t *kb = malloc(ix->kbytes + 1);
  if (vals == NULL || kb == NULL) {
    free(vals); free(kb);
    return err_oom(e);
  }
  Status st = STATUS_OK;
  for (uint32_t r = ix->synced; r < t->count && st == STATUS_OK; r++) {
    for (uint32_t i = 0; i < ix->nb; i++) vals[ix->cols[i]] = tcell(t, (int)ix->cols[i], r);
    uint64_t h = hash_cells(t, ix->cols, ix->nb, vals);
    pack_key(t, ix, vals, kb);
    if (ix->scap == 0 || ((size_t)ix->nbk + 1) * 2 > ix->scap) {
      st = slots_grow(ix, e);
      if (st != STATUS_OK) break;
    }
    size_t i = h & (ix->scap - 1);
    uint32_t bi = UINT32_MAX;
    while (ix->used[i]) {
      if (ix->hash[i] == h && memcmp(ix->keys + (size_t)ix->bk[i] * ix->kbytes, kb, ix->kbytes) == 0) {
        bi = ix->bk[i];
        break;
      }
      i = (i + 1) & (ix->scap - 1);
    }
    if (bi == UINT32_MAX) {
      if (ix->nbk == ix->bkcap) {
        uint32_t nc = ix->bkcap == 0 ? 256 : ix->bkcap * 2;
        Bucket *nbk = realloc(ix->bks, (size_t)nc * sizeof *nbk);
        if (nbk == NULL) { st = err_oom(e); break; }
        memset(nbk + ix->bkcap, 0, (size_t)(nc - ix->bkcap) * sizeof *nbk);
        ix->bks = nbk;
        uint8_t *nkeys = realloc(ix->keys, (size_t)nc * ix->kbytes + 1);
        if (nkeys == NULL) { st = err_oom(e); break; }
        ix->keys = nkeys;
        ix->bkcap = nc;
      }
      bi = ix->nbk++;
      ix->bks[bi].n = 0;
      memcpy(ix->keys + (size_t)bi * ix->kbytes, kb, ix->kbytes);
      ix->used[i] = 1;
      ix->hash[i] = h;
      ix->bk[i] = bi;
    }
    st = bucket_push(ix, &ix->bks[bi], r, t, e);
    ix->synced = r + 1;
  }
  free(vals);
  free(kb);
  return st;
}

const Bucket *index_find(const Index *ix, const Table *t, const I128 *vals_by_col, uint8_t *key) {
  if (ix->direct_sym) return index_find_sym(ix, (uint32_t)vals_by_col[ix->cols[0]]);
  if (ix->scap == 0) return NULL;
  uint64_t h = hash_cells(t, ix->cols, ix->nb, vals_by_col);
  pack_key(t, ix, vals_by_col, key);
  size_t i = h & (ix->scap - 1);
  while (ix->used[i]) {
    if (ix->hash[i] == h && memcmp(ix->keys + (size_t)ix->bk[i] * ix->kbytes, key, ix->kbytes) == 0) return &ix->bks[ix->bk[i]];
    i = (i + 1) & (ix->scap - 1);
  }
  return NULL;
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
