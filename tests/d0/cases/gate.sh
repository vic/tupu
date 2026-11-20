#!/bin/sh
# The whole gate (rulings S31, S85, S88, S89): every family in fixed order as a fixed list of RANGE slices, one run.sh call
# per slice under `timeout 60`; the output is concatenated and compared by check.sh with expected.txt and
# expected-souffle64.txt. A slice that exits 124 is split in two in the list below (never a higher cap) and the split is
# reported. The list of a family must cover the job positions 1..N exactly, where N is the number of its lines in
# expected.txt plus expected-souffle64.txt, and the queue of the family must have N jobs; otherwise gate.sh stops with a
# named message. Prints nothing when the gate is clean (a named message on stdout otherwise).
# Usage: SOUFFLE=... nix-shell -p mcpp --run 'sh gate.sh'     SLICE_TIMES=FILE appends "family range exit wall" per slice.
D=$(dirname "$0")

# family: the slices, and the expected-line prefixes of the family (the family `rows` is part of arith)
slices() {
  case $1 in
    bounds) echo "1-11 12-22 23-32" ;;
    memory) echo "1-31 32-62 63-93 94-125" ;;
    arith) echo "${ARITH_SLICES:-1-11 12-22 23-33 34-44 45-55 56-66 67-77}" ;;
    contracts) echo "1-18 19-36" ;;
    flow) echo "1-25 26-50" ;;
    capabilities) echo "1-28" ;;
    names) echo "1-36" ;;
    tests) echo "1-20" ;;
    types) echo "1-35" ;;
    stack) echo "1-18" ;;
    refuse) echo "1-8" ;;
    corpus) echo "1-30 31-60 61-90 91-110" ;;
  esac
}
prefix() { case $1 in arith) echo 'arith|rows' ;; *) echo "$1" ;; esac; }
lines_of() { # the number of expected lines of a family
  a=$(grep -cE "^($(prefix "$1")) " "$D/expected.txt"); b=$(grep -v '^#' "$D/expected-souffle64.txt" | grep -cE "^($(prefix "$1")) "); echo $((a + b))
}
FAMILIES="bounds memory arith contracts flow capabilities names tests types stack refuse corpus"

# ruling S88: every list covers 1..N exactly, and every queue has N jobs
for fam in $FAMILIES; do
  N=$(lines_of "$fam"); next=1
  for r in $(slices "$fam"); do
    lo=${r%-*}; hi=${r#*-}
    if [ "$lo" -ne "$next" ] || [ "$hi" -lt "$lo" ]; then
      echo "gate.sh: $fam slices do not cover 1..$N exactly: the slice $r follows position $((next - 1))"; exit 2
    fi
    next=$((hi + 1))
  done
  if [ "$next" -ne $((N + 1)) ]; then echo "gate.sh: $fam slices end at position $((next - 1)) but the $fam family has $N lines"; exit 2; fi
  J=$(COUNT=1 timeout 60 sh "$D/run.sh" "$fam" 2>/dev/null)
  if [ "$J" -ne "$N" ]; then echo "gate.sh: the $fam queue has $J jobs but the expected files have $N $fam lines"; exit 2; fi
done

[ -n "${COVERAGE_ONLY:-}" ] && exit 0   # S88 check only (no slice runs)
T=$(mktemp); trap 'rm -f "${T:?}"' EXIT
one() { # one slice under the cap; any failure is named
  s=$(date +%s)
  RANGE=$2 timeout 60 sh "$D/run.sh" "$1" > "$T" 2>/dev/null; rc=$?
  if [ -n "${SLICE_TIMES:-}" ]; then echo "$1 $2 exit=$rc wall=$(($(date +%s) - s))s" >> "$SLICE_TIMES"; fi
  if [ "$rc" -ne 0 ]; then echo "gate.sh: run.sh $1 RANGE=$2 exited $rc (124 = the cap of 60 s): split this slice in two"; fi
  cat "$T"
}
for fam in $FAMILIES; do for r in $(slices "$fam"); do one "$fam" "$r"; done; done | cut -c1-230 | sh "$D/check.sh"
