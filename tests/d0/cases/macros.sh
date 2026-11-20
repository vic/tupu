#!/bin/sh
# Oracle of numbers.dl: every macro against exact arithmetic (bc) on all triples of 28 operands that include the
# extremes of the signed 64-bit range. Run from this directory:
#   timeout 280 nix-shell -p souffle mcpp bc --run 'sh macros.sh'      (SOUFFLE=path/to/64-bit/souffle to choose a build)
# Prints one line: the counts and `macros equal` or the first differences.
set -eu
W=${TMPDIR:-/tmp}/stigi-macros.$$
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
SOUFFLE=${SOUFFLE:-souffle}
E="-9223372036854775808 -9223372036854775807 -4611686018427387905 -4611686018427387904 -4294967296 -3 -2 -1 0 1 2 3 4294967296 4611686018427387904 4611686018427387905 9223372036854775806 9223372036854775807 3037000499 3037000500 -3037000499 -3037000500 2147483648 -2147483648 3074457345618258602 3074457345618258603 -3074457345618258602 7 -7"
for a in $E; do for b in $E; do for c in $E; do printf '%s\t%s\t%s\n' "$a" "$b" "$c"; done; done; done > "$W/t.facts"
n=$(wc -l < "$W/t.facts")
# the exact answers: bc has no booleans in output, so one line per true triple
awk -F'\t' '{
  a=$1; b=$2; c=$3
  printf "if (%s + %s <= %s) print \"sumle\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
  printf "if (%s + %s >= %s) print \"sumge\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
  printf "if (%s - %s <= %s) print \"diffle\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
  printf "if (%s - %s >= %s) print \"diffge\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
  printf "if (%s * %s <= %s) print \"mulle\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
  printf "if (%s * %s >= %s) print \"mulge\\t%s\\t%s\\t%s\\n\"\n", a, b, c, a, b, c
}' "$W/t.facts" > "$W/exact.bc"
echo quit >> "$W/exact.bc"
BC_LINE_LENGTH=0 bc -q "$W/exact.bc" | sort > "$W/exact.txt"
mkdir -p "$W/f"; cp "$W/t.facts" "$W/f/t.facts"
check() { $SOUFFLE $1 -F "$W/f" ../macros.dl 2>&1 | awk '/^====/ { n++; next } n == 1 { print }' | sort > "$W/souffle.txt"; }
check ""
if cmp -s "$W/exact.txt" "$W/souffle.txt"; then
  echo "macros equal: $n triples, $(wc -l < "$W/exact.txt") true rows"
else
  echo "macros DIFFER: exact $(wc -l < "$W/exact.txt") rows, souffle $(wc -l < "$W/souffle.txt") rows"
  diff "$W/exact.txt" "$W/souffle.txt" | head -20
fi
# armed control: with the plain sum in SUMLE the same comparison must differ from the exact answer
check -MMUTANT_PLAINMACRO
if cmp -s "$W/exact.txt" "$W/souffle.txt"; then echo "control FAILED: the plain sum was not seen"; else echo "control armed: the plain sum differs"; fi
