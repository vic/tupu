/* tupu: standalone Datalog engine. C0 skeleton: argument handling and the typed result path only.
   Every outcome is a value of Result; main maps it to an exit status and a message (ruling S79). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { STATUS_COMPLETE, STATUS_REJECTED } Status;

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
  CODE_ENGINE_NOT_BUILT /* C0 only: removed when the engine lands (step 3) */
} Code;

typedef struct {
  Status status;
  Code code; /* meaningful only when status == STATUS_REJECTED */
  const char *file; /* NULL: a code of the whole run, printed as "-" */
  long line;
  const char *text;
} Result;

typedef struct {
  const char *facts_dir;
  const char *out_target;
  const char *program;
  long budget_rows;
} Args;

static const char *code_name(Code c) {
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
  case CODE_ENGINE_NOT_BUILT: return "engine-not-built";
  }
  return "usage"; /* unreachable: the switch covers the closed enum */
}

static Result rejected(Code c, const char *text) {
  Result r = { STATUS_REJECTED, c, NULL, 0, text };
  return r;
}

static Result complete(void) {
  Result r = { STATUS_COMPLETE, CODE_USAGE, NULL, 0, "" };
  return r;
}

static Result parse_args(int argc, char **argv, Args *a) {
  a->facts_dir = NULL;
  a->out_target = NULL;
  a->program = NULL;
  a->budget_rows = 1000000; /* the floor of I4 */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) a->facts_dir = argv[++i];
    else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) a->out_target = argv[++i];
    else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      char *end;
      long v = strtol(argv[++i], &end, 10);
      if (*end != '\0' || end == argv[i] || v < 0) return rejected(CODE_USAGE, "-b needs a row count");
      a->budget_rows = v;
    } else if (a->program == NULL && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0)) a->program = argv[i];
    else return rejected(CODE_USAGE, "tupu -F DIR -D (-|DIR) [-b ROWS] PROGRAM");
  }
  if (a->facts_dir == NULL || a->out_target == NULL || a->program == NULL)
    return rejected(CODE_USAGE, "tupu -F DIR -D (-|DIR) [-b ROWS] PROGRAM");
  return complete();
}

static Result run(const Args *a) {
  (void)a;
  return rejected(CODE_ENGINE_NOT_BUILT, "the engine lands in C1");
}

int main(int argc, char **argv) {
  Args a;
  Result r = parse_args(argc, argv, &a);
  if (r.status == STATUS_COMPLETE) r = run(&a);
  if (r.status == STATUS_REJECTED) {
    fprintf(stderr, "rejected %s %s:%ld: %s\n", code_name(r.code), r.file ? r.file : "-", r.line, r.text);
    return r.code == CODE_USAGE ? 2 : 1;
  }
  return 0;
}
