#!/bin/sh
# O3 (S90 6): the D0 suite. The stream of run.sh (every family in the order of gate.sh, arith in the four RANGE slices of S88,
# each call under timeout 60) through tests/souffle-shim.sh, checked like cases/check.sh: the lines of expected-exact.txt are
# compared in their own file, every other line against expected.txt. The refuse family is O2 (tests/o2.sh).
# Prints nothing but the summary line when the diff is empty. usage: BIN=build/tupu sh tests/o3.sh
set -u
BIN=${BIN:?BIN is not set}
REAL_PATH=$PATH; export REAL_PATH
ROOT=$(pwd)
PATH=$ROOT/tests/bin:$PATH; export PATH
W=$(mktemp -d "${TMPDIR:-/tmp}/o3.XXXXXX"); trap 'rm -rf "$W"' EXIT
export TUPU=$ROOT/$BIN SOUFFLE=$ROOT/tests/souffle-shim.sh
D=$ROOT/tests/d0/cases
start=$(date +%s)
: > "$W/stream"
one() { # family [range]: a run that fails or hits the 60 s cap is named, never waited for
  RANGE=${2:-} nice -n 10 timeout 60 sh "$D/run.sh" "$1" 2> /dev/null | cut -c1-230 >> "$W/stream"
  rc=$?; [ $rc = 0 ] || echo "O3 run.sh $1 ${2:+RANGE=$2 }exit $rc"
}
for fam in bounds memory arith contracts flow capabilities names tests types stack corpus; do
  if [ "$fam" = arith ]; then for r in 1-20 21-40 41-60 61-77; do ( cd "$D" && one arith "$r" ); done; else ( cd "$D" && one "$fam" ); fi
done
grep -v '^#' "$D/expected-exact.txt" > "$W/exact"
grep -v '^refuse ' "$D/expected.txt" > "$W/exp"
grep -vxFf "$W/exact" "$W/stream" > "$W/a"
grep -xFf "$W/exact" "$W/stream" > "$W/b"
diff "$W/a" "$W/exp" > "$W/d1"; diff "$W/b" "$W/exact" > "$W/d2"
cat "$W/d1" "$W/d2" | head -${DIFFLINES:-12} | cut -c1-260
echo "O3 stream=$(wc -l < "$W/stream") expected=$(wc -l < "$W/exp") exact=$(wc -l < "$W/exact") difflines=$(cat "$W/d1" "$W/d2" | wc -l) wall=$(( $(date +%s) - start ))s"
[ ! -s "$W/d1" ] && [ ! -s "$W/d2" ]
