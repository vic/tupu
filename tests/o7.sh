#!/bin/sh
# O7: the model does not depend on the order of rules and facts. Every D0 family (the shim) and every O1 program runs
# unshuffled and with the program clauses and every facts file shuffled under seeds 1, 2, 3 (tests/shuffle.sh);
# stdout of each run must be byte-equal to the unshuffled run. usage: BIN=build/tupu sh tests/o7.sh
set -u
BIN=${BIN:?BIN is not set}
ROOT=$(pwd)
O=/tmp/claude-1000/o7.$$; mkdir -p "$O"
rc=0
for fam in bounds memory arith contracts flow capabilities names tests types stack corpus; do
  for seed in "" 1 2 3; do
    ( cd tests/d0/cases && TUPU=$ROOT/$BIN SOUFFLE=$ROOT/tests/souffle-shim.sh TUPU_SHUFFLE=$seed sh run.sh "$fam" 2>/dev/null ) > "$O/$fam.${seed:-0}"
  done
  n=$(wc -l < "$O/$fam.0"); bad=0
  for seed in 1 2 3; do cmp -s "$O/$fam.0" "$O/$fam.$seed" || { bad=$((bad+1)); echo "O7 $fam seed $seed DIFFERS: $(diff "$O/$fam.0" "$O/$fam.$seed" | head -2 | cut -c1-160 | tr '\n' ' ')"; }; done
  echo "O7 d0 $fam lines=$n seeds_differing=$bad"; [ $bad = 0 ] || rc=1
done
for seed in 1 2 3; do
  out=$(SEED=$seed BIN=$BIN sh tests/o1.sh 2>&1)
  echo "O7 o1 seed $seed: programs=$(echo "$out" | grep -c 'relations_equal') failing=$(echo "$out" | grep -c -e DIFFERS -e REFUSED)"
  echo "$out" | grep -q -e DIFFERS -e REFUSED && rc=1
done
rm -rf "$O"
exit $rc
