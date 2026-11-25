#!/bin/sh
# Lattice oracle (subsumption R(k.., a) <= R(k.., b) :- a <= b): the longest path costs of a 40-node DAG computed with
# the lattice equal the max over ALL path rows of the plain program (two independent paths); the lattice output is
# byte-identical for -j 1, -j 2 and -j 6 (also for monotone readers in other relations: far, capped); every reader that
# could see a dominated row is refused (fail closed).
# MUT=build/tupu-mut-LATTICEDIR (the table keeps the worse row) must make the oracle differ.
# usage: BIN=build/tupu [MUT=build/tupu-mut-LATTICEDIR] sh tests/lattice.sh
set -u
BIN=${BIN:?BIN is not set}
D=tests/lattice
T=$(mktemp -d "${TMPDIR:-/tmp}/lat.XXXXXX"); trap 'rm -rf "$T"' EXIT
rc=0
timeout 20 "$BIN" -j 1 -F $D -D - $D/plain.dl > "$T/plain" 2>&1 || rc=1
for j in 1 2 6; do timeout 20 "$BIN" -j $j -F $D -D - $D/lattice.dl > "$T/lat.$j" 2>&1 || rc=1; done
if [ -s "$T/plain" ] && cmp -s "$T/plain" "$T/lat.1" && cmp -s "$T/lat.1" "$T/lat.2" && cmp -s "$T/lat.1" "$T/lat.6"; then
  echo "LAT exact rows=$(wc -l < "$T/plain") j1_j2_j6=equal"
else
  echo "LAT exact DIFFERS"; rc=1
fi
base=$(grep -v 'longest\|far\|capped' $D/lattice.dl)
refuse() { # name extra-lines
  printf '%s\n%s\n' "$base" "$2" > "$T/$1.dl"
  if timeout 20 "$BIN" -F $D -D - "$T/$1.dl" > "$T/$1.out" 2>&1; then echo "LAT refuse $1 ACCEPTED"; rc=1
  elif grep -q 'lattice relation\|subsumption' "$T/$1.out"; then echo "LAT refuse $1 ok"
  else echo "LAT refuse $1 WRONG-REASON: $(head -1 "$T/$1.out")"; rc=1; fi
}
refuse negation '.decl unreached(k: symbol)
unreached(k) :- root(r), cost(k, _), !path(r, k, _).
.output unreached'
refuse copy '.decl copy(r: symbol, k: symbol, c: number)
copy(r, k, c) :- path(r, k, c).
.output copy'
refuse minOfMax '.decl shortest(r: symbol, k: symbol, c: number)
shortest(r, k, n) :- root(r), cost(k, _), n = min c : { path(r, k, c) }.
.output shortest'
refuse decreasing 'path(r, b, 1000 - c) :- path(r, a, c), edge(a, b).'
refuse compared 'path(r, b, c + x) :- path(r, a, c), edge(a, b), cost(b, x), c < 10.'
refuse antitoneReader '.decl near(r: symbol, k: symbol)
near(r, k) :- path(r, k, c), c + 1 <= 30.
.output near'
refuse equality '.decl exactly(r: symbol, k: symbol)
exactly(r, k) :- path(r, k, c), c = 30.
.output exactly'
refuse joined '.decl same(r: symbol, k: symbol)
same(r, k) :- path(r, k, c), cost(k, c).
.output same'
refuse output '.output path'
refuse twoColumns '.decl pair(a: number, b: number)
pair(x, y) <= pair(z, w) :- x <= z.'
if [ -n "${MUT:-}" ]; then
  timeout 20 "$MUT" -j 1 -F $D -D - $D/lattice.dl > "$T/mut" 2>&1
  if cmp -s "$T/plain" "$T/mut"; then echo "LAT mutant SURVIVED"; rc=1; else echo "LAT mutant killed"; fi
fi
echo "LAT failing=$([ $rc = 0 ] && echo 0 || echo some)"
exit $rc
