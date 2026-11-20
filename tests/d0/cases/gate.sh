#!/bin/sh
# The whole gate: every family in fixed order, one run.sh call each under `timeout 280`, output concatenated (plumbing,
# ruling S31), then compared by check.sh with expected.txt and expected-souffle64.txt (ruling S85).
# A family that does not fit the cap runs as a fixed list of RANGE slices (ruling S88); the list must cover the job
# positions 1..N of the family exactly, where N is the number of its lines in expected.txt plus expected-souffle64.txt.
# Prints nothing when the gate is clean; a named message (on stdout) when a slice list or a run fails.
# Usage: SOUFFLE=... nix-shell -p mcpp --run 'sh gate.sh'
D=$(dirname "$0")
# the slices of arith (ARITH_SLICES in the environment replaces the list: used only by the armed control of S88)
ARITH_SLICES=${ARITH_SLICES:-"1-20 21-40 41-60 61-77"}

lines_of() { # family-token regex: the number of expected lines of the family (the family `rows` is part of arith)
  a=$(grep -cE "^($1) " "$D/expected.txt"); b=$(grep -v '^#' "$D/expected-souffle64.txt" | grep -cE "^($1) "); echo $((a + b))
}

# S88: the list of slices covers 1..N exactly, and the queue has N jobs
N=$(lines_of 'arith|rows')
next=1
for r in $ARITH_SLICES; do
  lo=${r%-*}; hi=${r#*-}
  if [ "$lo" -ne "$next" ] || [ "$hi" -lt "$lo" ]; then
    echo "gate.sh: arith slices do not cover 1..$N exactly: the slice $r follows position $((next - 1))"; exit 2
  fi
  next=$((hi + 1))
done
if [ "$next" -ne $((N + 1)) ]; then echo "gate.sh: arith slices end at position $((next - 1)) but the arith family has $N lines"; exit 2; fi
J=$(COUNT=1 sh "$D/run.sh" arith 2>/dev/null)
if [ "$J" -ne "$N" ]; then echo "gate.sh: the arith queue has $J jobs but the expected files have $N arith lines"; exit 2; fi

T=$(mktemp); trap 'rm -f "${T:?}"' EXIT
one() { # one run.sh call under the cap; any failure is named
  RANGE=${RANGE:-} timeout 280 sh "$D/run.sh" "$1" > "$T" 2>/dev/null; rc=$?
  if [ "$rc" -ne 0 ]; then echo "gate.sh: run.sh $1 ${RANGE:+RANGE=$RANGE }exited $rc (124 = the cap of 280 s)"; fi
  cat "$T"
}
for fam in bounds memory arith contracts flow capabilities names tests types stack refuse corpus; do
  if [ "$fam" = arith ]; then for r in $ARITH_SLICES; do RANGE=$r one arith; done; else one "$fam"; fi
done | cut -c1-230 | sh "$D/check.sh"
