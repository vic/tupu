#!/bin/sh
# Per obligation code: the cases where it is owed and cleared (accepted) and owed and not cleared (rejected).
# Usage: SOUFFLE=... nix-shell -p mcpp --run 'sh cover.sh'   (one capped run.sh call per family; refusal cases have no codes)
for fam in bounds memory arith contracts flow capabilities names tests types stack corpus; do COVER=1 sh run.sh "$fam" 2>/dev/null; done |
  sort -u | awk -F'\t' '{ n[$1 "\t" $2]++; code[$1] = 1 } END { for (c in code) printf "%s\t%d\t%d\n", c, n[c "\taccepted"], n[c "\trejected"] }' | sort
