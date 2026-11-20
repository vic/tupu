#!/bin/sh
# O3: the D0 suite. run.sh of the snapshot runs unchanged with the shim in place of souffle; every family's lines
# are diffed with the lines of tests/d0/cases/expected.txt. The refuse family is O2 (tests/o2.sh).
# usage: BIN=build/tupu sh tests/o3.sh [family...]
set -u
BIN=${BIN:?BIN is not set}
ROOT=$(pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/o3.XXXXXX"); trap 'rm -rf "$W"' EXIT
export TUPU=$ROOT/$BIN SOUFFLE=$ROOT/tests/souffle-shim.sh
fams=${*:-bounds memory arith contracts flow capabilities names tests types stack corpus}
rc=0
cd tests/d0/cases || exit 2
for f in $fams; do
  timeout 280 sh run.sh "$f" 2> "$W"/err | cut -c1-230 > "$W"/out
  labels=$(awk '{print $1}' "$W"/out | sort -u | paste -sd'|')
  grep -E "^($labels) " expected.txt > "$W"/exp
  nout=$(wc -l < "$W"/out); nexp=$(wc -l < "$W"/exp)
  if diff "$W"/out "$W"/exp > "$W"/diff; then echo "O3 $f lines=$nout expected=$nexp diff=empty";
  else echo "O3 $f lines=$nout expected=$nexp diff=$(grep -c '^<' "$W"/diff) lines differ"; rc=1; head -${DIFFLINES:-6} "$W"/diff | cut -c1-260; fi
done
exit $rc
