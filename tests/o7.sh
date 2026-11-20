#!/bin/sh
# O7: the model does not depend on the order of rules and facts. Every D0 family (through the shim, # S88: every family in slices of 15 jobs, each run.sh call under timeout 60) and every O1 program runs unshuffled and with the program clauses and every facts file
# shuffled under seeds 1, 2, 3 (tests/shuffle.sh); stdout of each shuffled run must equal the unshuffled run.
# A slice that exits non-zero is named, never waited for. usage: BIN=build/tupu sh tests/o7.sh [family...]
set -u
BIN=${BIN:?BIN is not set}
REAL_PATH=$PATH; export REAL_PATH
ROOT=$(pwd)
PATH=$ROOT/tests/bin:$PATH; export PATH
O=$(mktemp -d "${TMPDIR:-/tmp}/o7.XXXXXX"); trap 'rm -rf "$O"' EXIT
fams=${*:-bounds memory arith contracts flow capabilities names tests types stack corpus}
rc=0
slice() { # family range seed outfile
  ( cd tests/d0/cases && RANGE=$2 TUPU=$ROOT/$BIN SOUFFLE=$ROOT/tests/souffle-shim.sh TUPU_SHUFFLE=$3 nice -n 10 timeout 60 sh run.sh "$1" 2>/dev/null >> "$4" )
  r=$?; [ $r = 0 ] || { echo "O7 run.sh $1 ${2:+RANGE=$2 }seed=${3:-none} exit $r"; rc=1; }
}
for fam in $fams; do
  N=$( cd tests/d0/cases && COUNT=1 SOUFFLE=$ROOT/tests/souffle-shim.sh TUPU=$ROOT/$BIN sh run.sh "$fam" 2>/dev/null ); ranges=""; lo=1
  while [ "$lo" -le "${N:-0}" ]; do hi=$((lo + 14)); [ "$hi" -gt "$N" ] && hi=$N; ranges="$ranges $lo-$hi"; lo=$((hi + 1)); done
  for seed in "" 1 2 3; do
    : > "$O/$fam.${seed:-0}"
    if [ -n "$ranges" ]; then for r in $ranges; do slice "$fam" "$r" "$seed" "$O/$fam.${seed:-0}"; done; else slice "$fam" "" "$seed" "$O/$fam.${seed:-0}"; fi
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
exit $rc
