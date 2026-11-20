/* tupu: standalone Datalog engine. C0 skeleton: argument handling and the typed result path only.
   Every outcome is a value of Result; main maps it to an exit status and a message. */
#include <stdio.h>
#include <string.h>

typedef enum { STATUS_COMPLETE, STATUS_REJECTED } Status;

typedef enum {
  CODE_USAGE,
  CODE_ENGINE_NOT_BUILT /* C0 only: the reader and the engine land in C1 */
} Code;

typedef struct {
  Status status;
  Code code; /* meaningful only when status == STATUS_REJECTED */
} Result;

typedef struct {
  const char *facts_dir;
  const char *out_target;
} Args;

static const char *code_name(Code c) {
  switch (c) {
  case CODE_USAGE: return "usage";
  case CODE_ENGINE_NOT_BUILT: return "engine-not-built";
  }
  return "usage"; /* unreachable: the switch covers the closed enum */
}

static Result rejected(Code c) {
  Result r = { STATUS_REJECTED, c };
  return r;
}

static Result parse_args(int argc, char **argv, Args *a) {
  a->facts_dir = NULL;
  a->out_target = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) a->facts_dir = argv[++i];
    else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) a->out_target = argv[++i];
    else return rejected(CODE_USAGE);
  }
  if (a->facts_dir == NULL || a->out_target == NULL) return rejected(CODE_USAGE);
  Result ok = { STATUS_COMPLETE, CODE_USAGE };
  return ok;
}

static Result run(const Args *a) {
  (void)a;
  return rejected(CODE_ENGINE_NOT_BUILT);
}

int main(int argc, char **argv) {
  Args a;
  Result r = parse_args(argc, argv, &a);
  if (r.status == STATUS_COMPLETE) r = run(&a);
  if (r.status == STATUS_REJECTED) {
    fprintf(stderr, "rejected: %s\n", code_name(r.code));
    return 2;
  }
  return 0;
}
