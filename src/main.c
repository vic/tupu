/* tupu: standalone Datalog engine. Every outcome is a Status plus an Err; main maps it to the exit status (S79):
   0 complete, 1 rejected, 2 usage. Nothing is written to stdout when the run is rejected. */
#include "ast.h"
#include "engine.h"
#include "out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *facts_dir;
  const char *out_target;
  const char *program;
  const char *prov_dir;
  uint64_t budget;
} Args;

static Status parse_args(int argc, char **argv, Args *a, Err *e) {
  const char *usage = "tupu -F DIR -D (-|DIR) [-b ROWS] [-P DIR] PROGRAM";
  memset(a, 0, sizeof *a);
  a->budget = 1000000; /* the floor of I4 */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-F") == 0 && i + 1 < argc && a->facts_dir == NULL) a->facts_dir = argv[++i];
    else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc && a->out_target == NULL) a->out_target = argv[++i];
    else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc && a->prov_dir == NULL) a->prov_dir = argv[++i];
    else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      char *end;
      unsigned long long v = strtoull(argv[++i], &end, 10);
      if (*end != '\0' || end == argv[i] || argv[i][0] == '-') return err_set(e, CODE_USAGE, NULL, 0, "-b needs a row count");
      a->budget = v;
    } else if (a->program == NULL && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0)) a->program = argv[i];
    else return err_set(e, CODE_USAGE, NULL, 0, "%s", usage);
  }
  if (a->facts_dir == NULL || a->out_target == NULL || a->program == NULL) return err_set(e, CODE_USAGE, NULL, 0, "%s", usage);
  return STATUS_OK;
}

static Status read_program(const char *path, char **text, size_t *len, Err *e) {
  FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
  if (f == NULL) return err_set(e, CODE_IO_FAILED, NULL, 0, "cannot open %s", path);
  size_t cap = 1 << 16, n = 0;
  char *b = malloc(cap);
  if (b == NULL) return err_oom(e);
  for (;;) {
    if (n == cap) {
      char *nb = realloc(b, cap * 2);
      if (nb == NULL) {
        free(b);
        return err_oom(e);
      }
      b = nb;
      cap *= 2;
    }
    size_t got = fread(b + n, 1, cap - n, f);
    n += got;
    if (got == 0) break;
  }
  bool bad = ferror(f) != 0;
  if (f != stdin) (void)fclose(f);
  if (bad) {
    free(b);
    return err_set(e, CODE_IO_FAILED, NULL, 0, "read failed on %s", path);
  }
  *text = b;
  *len = n;
  return STATUS_OK;
}

static Status run(const Args *a, Err *e) {
  char *text = NULL;
  size_t len = 0;
  if (read_program(a->program, &text, &len, e) != STATUS_OK) return STATUS_ERR;
  Program prog;
  program_init(&prog);
  Engine eng;
  bool open = false;
  Status st = parse_program(&prog, text, len, strcmp(a->program, "-") == 0 ? "<stdin>" : a->program, e);
  if (st == STATUS_OK) st = check_program(&prog, e);
  if (st == STATUS_OK) {
    st = engine_open(&eng, &prog, a->facts_dir, a->budget, a->prov_dir != NULL, e);
    open = true;
  }
  if (st == STATUS_OK) st = engine_eval(&eng, e);
  if (st == STATUS_OK && a->prov_dir != NULL) st = engine_replay(&eng, e);
  if (st == STATUS_OK) st = out_write(&eng, a->out_target, e);
  if (st == STATUS_OK && a->prov_dir != NULL) st = out_write_prov(&eng, a->prov_dir, e);
  if (open) engine_close(&eng);
  program_free(&prog);
  free(text);
  return st;
}

int main(int argc, char **argv) {
  Args a;
  Err e;
  Status st = parse_args(argc, argv, &a, &e);
  if (st == STATUS_OK) st = run(&a, &e);
  if (st == STATUS_ERR) {
    if (e.file == NULL) fprintf(stderr, "rejected %s -: %s\n", code_name(e.code), e.text);
    else fprintf(stderr, "rejected %s %s:%ld: %s\n", code_name(e.code), e.file, e.line, e.text);
    return e.code == CODE_USAGE ? 2 : 1;
  }
  return 0;
}
