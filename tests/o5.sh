#!/bin/sh
# O5 (and the O6 mutant): every engine mutant is a release build with -DMUTANT_NAME (src/*.c, #ifdef). For each, run the
# own cases, O1 with -P, the counters check and three D0 families, and print what flipped against the unmutated build.
# usage: sh tests/o5.sh   (inside nix-shell; builds build/tupu-mut-NAME)
set -u
rc=0
MUTS=${*:-WRAPADD NOBUDGET NEGEARLY FULLDELTA IDXDROP DEDUPREL PROVBODY}
FAMS="tests types contracts arith"
ROOT=$(pwd)
O=$(mktemp -d "${TMPDIR:-/tmp}/o5.XXXXXX")
d0() { # BIN outfile
  ( cd tests/d0/cases && TUPU=$ROOT/$1 SOUFFLE=$ROOT/tests/souffle-shim.sh TUPU_PROV=1 sh run.sh $FAMS 2>/dev/null | cut -c1-230 ) > "$2"
  # run.sh takes one family: run them one by one
  : > "$2"
  for f in $FAMS; do ( cd tests/d0/cases && TUPU=$ROOT/$1 SOUFFLE=$ROOT/tests/souffle-shim.sh TUPU_PROV=1 sh run.sh "$f" 2>/dev/null | cut -c1-230 ) >> "$2"; done
}
d0 build/tupu "$O/base.d0"
echo "O5 baseline: d0 families [$FAMS] lines=$(wc -l < "$O/base.d0")"
for m in $MUTS; do
  just mutant "$m" > /dev/null 2>&1 || { echo "O5 $m BUILD FAILED"; continue; }
  B=build/tupu-mut-$m
  c0=$(BIN=$B sh tests/run.sh 2>&1 | grep -c ' FAIL ')
  o1=$(PROVFLAG=1 BIN=$B sh tests/o1.sh 2>&1 | grep -c -e DIFFERS -e REFUSED)
  ct=$(BIN=$B sh tests/counters.sh 2>&1 | grep -c FAIL)
  d0 "$B" "$O/mut.d0"
  fl=$(diff "$O/base.d0" "$O/mut.d0" | grep -c '^>')
  ex=$(diff "$O/base.d0" "$O/mut.d0" | grep '^>' | head -1 | cut -c1-150)
  ex0=$(BIN=$B sh tests/run.sh 2>&1 | grep ' FAIL ' | head -1 | cut -c1-150)
  echo "O5 $m: own-cases-failing=$c0 o1-programs-failing=$o1 counters-failing=$ct d0-lines-flipped=$fl"
  [ $((c0 + o1 + ct + fl)) -gt 0 ] || { echo "O5 $m NOT KILLED: no oracle row flipped"; rc=1; }
  [ -n "$ex0" ] && echo "     own case: $ex0"
  [ -n "$ex" ] && echo "     d0 line:  $ex"
done
rm -rf "$O"
exit $rc
