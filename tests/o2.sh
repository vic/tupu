#!/bin/sh
# O2: refusals. D0 refuse programs with tests/d0/refuse-codes.tsv, the Souffle stratification tests (negation-cycle),
# and the Souffle dominance example (parse: .comp, .init, subsumption are outside S77).
# usage: BIN=build/tupu sh tests/o2.sh
set -u
BIN=${BIN:?BIN is not set}
T=$(mktemp -d "${TMPDIR:-/tmp}/o2.XXXXXX"); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
check() { # name program expected
  mcpp -e utf8 -W0 "$2" 2> /dev/null | timeout 1 "$BIN" -F "$T" -D - - > "$T/out" 2> "$T/err"; st=$?
  if [ "$3" = accepted ]; then got=accepted; [ $st = 0 ] || got=$(cut -d' ' -f2 "$T/err"); else got=$(cut -d' ' -f2 "$T/err"); [ $st = 1 ] || got="exit$st"; fi
  if [ "$got" = "$3" ]; then r=ok; pass=$((pass+1)); else r=FAIL; fail=$((fail+1)); fi
  echo "O2 $1 expected=$3 got=$got $r $(head -c 110 "$T/err" | tr '\n' ' ')"
}
tail -n +2 tests/d0/refuse-codes.tsv | while IFS="$(printf '\t')" read -r prog code; do
  check "d0/$prog" "tests/d0/cases/refuse/$prog.dl" "$code"
done > "$T/d0.lines"
cat "$T/d0.lines"
check souffle/rel_stratification tests/o2/rel_stratification/rel_stratification.dl negation-cycle
check souffle/rel_stratification2 tests/o2/rel_stratification2/rel_stratification2.dl negation-cycle
check souffle/dominance tests/o1/example/dominance/dominance.dl parse
