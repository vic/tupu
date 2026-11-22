#include "out.h"
#include "num.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *b;
  size_t n, cap;
} Text;

static Status text_put(Text *t, const char *s, size_t n, Err *e) {
  if (n == 0) return STATUS_OK;
  if (t->n + n > t->cap) {
    size_t nc = t->cap == 0 ? 4096 : t->cap * 2;
    while (nc < t->n + n) nc *= 2;
    char *nb = realloc(t->b, nc);
    if (nb == NULL) return err_oom(e);
    t->b = nb;
    t->cap = nc;
  }
  memcpy(t->b + t->n, s, n);
  t->n += n;
  return STATUS_OK;
}

typedef struct {
  size_t off, len;
  uint32_t row;
} Line;

static _Thread_local const char *sort_base;
static int cmp_line(const void *pa, const void *pb) {
  const Line *a = pa, *b = pb;
  size_t m = a->len < b->len ? a->len : b->len;
  int c = memcmp(sort_base + a->off, sort_base + b->off, m);
  if (c != 0) return c;
  if (a->len != b->len) return a->len < b->len ? -1 : 1;
  return (a->row > b->row) - (a->row < b->row);
}

/* render the rows of one relation as tab-separated text; lines sorted by the bytes of the row text */
static Status render(const Engine *x, size_t rel, Text *t, Line **out, Err *e) {
  const Table *tab = &x->tabs[rel];
  Line *ls = malloc(((size_t)tab->count + 1) * sizeof *ls);
  if (ls == NULL) return err_oom(e);
  const SymTab *sy = &x->prog->syms;
  for (uint32_t r = 0; r < tab->count; r++) {
    ls[r].off = t->n;
    ls[r].row = r;
    for (int c = 0; c < tab->ncols; c++) {
      Status st = STATUS_OK;
      if (c > 0) st = text_put(t, "\t", 1, e);
      if (st == STATUS_OK) {
        I128 v = tcell(tab, c, r);
        if (tab->kind[c] == BASE_SYMBOL) {
          st = text_put(t, sy->str[(uint32_t)v], sy->len[(uint32_t)v], e);
        } else {
          char nb[NUM_BUF];
          size_t n = num_format(v, nb);
          st = text_put(t, nb, n, e);
        }
      }
      if (st != STATUS_OK) {
        free(ls);
        return STATUS_ERR;
      }
    }
    ls[r].len = t->n - ls[r].off;
  }
  sort_base = t->b;
  qsort(ls, tab->count, sizeof *ls, cmp_line);
  *out = ls;
  return STATUS_OK;
}

typedef struct {
  FILE *f;
  char *buf;
  size_t n, cap;
  bool err;
} OutBuf;

static void ob_flush(OutBuf *o) {
  if (o->n > 0 && !o->err) {
    if (fwrite(o->buf, 1, o->n, o->f) != o->n) o->err = true;
  }
  o->n = 0;
}

static void ob_put(OutBuf *o, const char *s, size_t len) {
  if (o->err || len == 0) return;
  if (len > o->cap - o->n) {
    ob_flush(o);
    if (o->err) return;
    if (len >= o->cap) {
      if (fwrite(s, 1, len, o->f) != len) o->err = true;
      return;
    }
  }
  memcpy(o->buf + o->n, s, len);
  o->n += len;
}

Status out_write(const Engine *x, const char *target, Err *e) {
  const Program *p = x->prog;
  bool to_stdout = strcmp(target, "-") == 0;
  char *wbuf = malloc(65536);
  if (wbuf == NULL) return err_oom(e);
  OutBuf ob = { stdout, wbuf, 0, 65536, false };
  bool wrote_stdout = false;
  Status st = STATUS_OK;
  for (size_t r = 0; r < p->rels.n && st == STATUS_OK; r++) {
    const Relation *rel = &p->rels.p[r];
    if (!rel->is_output) continue;
    Text rows = { NULL, 0, 0 };
    Line *ls = NULL;
    st = render(x, r, &rows, &ls, e);
    if (st != STATUS_OK) {
      free(rows.b);
      break;
    }
    uint32_t count = x->tabs[r].count;
    if (to_stdout) {
      wrote_stdout = true;
      ob_put(&ob, "relation ", 9);
      ob_put(&ob, rel->name.s, rel->name.n);
      ob_put(&ob, "\n", 1);
      for (uint32_t i = 0; i < count && !ob.err; i++) {
        if (ls[i].len > 0) ob_put(&ob, rows.b + ls[i].off, ls[i].len);
        ob_put(&ob, "\n", 1);
      }
      if (ob.err) st = err_set(e, CODE_IO_FAILED, NULL, 0, "cannot write stdout");
    } else {
      size_t len = strlen(target) + rel->name.n + 8;
      char *path = malloc(len);
      if (path == NULL) st = err_oom(e);
      else {
        (void)snprintf(path, len, "%s/%.*s.csv", target, (int)rel->name.n, rel->name.s);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
          st = err_set(e, CODE_IO_FAILED, NULL, 0, "cannot write %s", path);
        } else {
          OutBuf fob = { f, wbuf, 0, 65536, false };
          for (uint32_t i = 0; i < count && !fob.err; i++) {
            if (ls[i].len > 0) ob_put(&fob, rows.b + ls[i].off, ls[i].len);
            ob_put(&fob, "\n", 1);
          }
          ob_flush(&fob);
          bool bad = fob.err || ferror(f) != 0;
          if (fclose(f) != 0 || bad) st = err_set(e, CODE_IO_FAILED, NULL, 0, "cannot write %s", path);
        }
        free(path);
      }
    }
    free(ls);
    free(rows.b);
  }
  if (st == STATUS_OK && to_stdout && wrote_stdout) {
    ob_flush(&ob);
    if (ob.err || ferror(stdout) != 0 || fflush(stdout) != 0) st = err_set(e, CODE_IO_FAILED, NULL, 0, "cannot write stdout");
  }
  free(wbuf);
  return st;
}

Status out_write_prov(const Engine *x, const char *dir, Err *e) {
  const Program *p = x->prog;
  Status st = STATUS_OK;
  for (size_t r = 0; r < p->rels.n && st == STATUS_OK; r++) {
    const Relation *rel = &p->rels.p[r];
    if (!rel->is_output) continue;
    const Table *tab = &x->tabs[r];
    Text rows = { NULL, 0, 0 };
    Line *ls = NULL;
    st = render(x, r, &rows, &ls, e);
    if (st != STATUS_OK) {
      free(rows.b);
      break;
    }
    Text body = { NULL, 0, 0 };
    for (uint32_t i = 0; i < tab->count && st == STATUS_OK; i++) {
      uint32_t row = ls[i].row;
      char tmp[160];
      st = text_put(&body, rows.b + ls[i].off, ls[i].len, e);
      if (st != STATUS_OK) break;
      int n;
      if (tab->org_kind[row] == ORG_INPUT) n = snprintf(tmp, sizeof tmp, "\tinput\t0\t");
      else if (tab->org_kind[row] == ORG_FACT) {
        const Fact *f = &p->facts.p[tab->org_ref[row]];
        n = snprintf(tmp, sizeof tmp, "\tfact %s:%ld\t0\t", f->file, f->line);
      } else n = snprintf(tmp, sizeof tmp, "\trule %u\t%u\t", tab->org_ref[row], tab->round[row]);
      st = text_put(&body, tmp, (size_t)n, e);
      if (st == STATUS_OK && tab->org_kind[row] == ORG_RULE) {
        const Variant *v = engine_variant_of(x, tab->org_ref[row]);
        const uint32_t *ids = tab->pool + tab->poff[row];
        for (int b = 0; b < (v ? v->natoms : 0) && st == STATUS_OK; b++) {
          const Relation *br = &p->rels.p[v->atom_rel[b]];
          if (b > 0) st = text_put(&body, " ", 1, e);
          if (st == STATUS_OK) st = text_put(&body, br->name.s, br->name.n, e);
          n = snprintf(tmp, sizeof tmp, "#%u", ids[b]);
          if (st == STATUS_OK) st = text_put(&body, tmp, (size_t)n, e);
        }
      }
      if (st == STATUS_OK) st = text_put(&body, "\n", 1, e);
    }
    free(ls);
    free(rows.b);
    if (st == STATUS_OK) {
      size_t len = strlen(dir) + rel->name.n + 8;
      char *path = malloc(len);
      if (path == NULL) st = err_oom(e);
      else {
        (void)snprintf(path, len, "%s/%.*s.prov", dir, (int)rel->name.n, rel->name.s);
        FILE *f = fopen(path, "wb");
        if (f == NULL || (body.n > 0 && fwrite(body.b, 1, body.n, f) != body.n) || fclose(f) != 0)
          st = err_set(e, CODE_IO_FAILED, NULL, 0, "cannot write %s", path);
        free(path);
      }
    }
    free(body.b);
  }
  return st;
}
