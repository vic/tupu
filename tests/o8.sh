#!/bin/sh
# S80 oracle: the output and the provenance files (-P: rule, round and body row ids, so the row order) are byte-identical for every thread count. Every O1 program and the wide case (4422 rows: more than one chunk of 4096) run with -j 1, -j 2 and -j 6; the three stdout streams must be `cmp`-equal, and the -j 1 run is the
# O1 oracle of tests/o1.sh (already checked). The default thread count (online CPUs) is compared with -j 1 on one program.
# usage: BIN=build/tupu sh tests/o8.sh
set -u
BIN=${BIN:?BIN is not set}
T=$(mktemp -d "${TMPDIR:-/tmp}/o8.XXXXXX"); trap 'rm -rf "$T"' EXIT
rc=0; n=0
one() { # name program facts  (-P replay of pointsto is too slow for the 60 s cap: no provenance there)
  for j in 1 2 6; do rm -rf "$T/p$j"; mkdir -p "$T/p$j"; timeout 20 nice -n 10 "$BIN" -j $j $(case "$1" in pointsto|budget) ;; wide) [ -n "${O8_SAN:-}" ] || echo "-P $T/p$j" ;; *)  echo "-P $T/p$j";; esac) -F "$3" -D - "$2" > "$T/out.$j" 2> "$T/err.$j"; echo $? >> "$T/err.$j"; done
  if cmp -s "$T/out.1" "$T/out.2" && cmp -s "$T/out.1" "$T/out.6" && cmp -s "$T/err.1" "$T/err.2" && cmp -s "$T/err.1" "$T/err.6" && diff -r "$T/p1" "$T/p2" > /dev/null && diff -r "$T/p1" "$T/p6" > /dev/null; then r=equal; else r=DIFFERS; rc=1; fi
  n=$((n+1)); echo "O8 $1 j1_j2_j6=$r rows=$(wc -l < "$T/out.1")"
}
for d in tests/o1/evaluation/* tests/o1/example/*; do
  name=$(basename "$d"); [ "$name" = dominance ] && continue
  prog=$d/$name.dl; [ -f "$prog" ] || prog=$d/$(ls "$d" | grep '\.dl$' | grep -v edb | head -1)
  facts=$d/facts; [ -d "$facts" ] || { facts=$T/empty; mkdir -p "$facts"; }
  mcpp -e utf8 -W0 "$prog" > "$T/pp.dl" 2> /dev/null
  one "$name" "$T/pp.dl" "$facts"
done
one wide tests/o8/wide/prog.dl tests/o8/wide
one arithOverflow tests/c0/arithOverflow/prog.dl tests/c0/arithOverflow
one budget tests/c0/budget/prog.dl tests/c0/budget
timeout 20 "$BIN" -F tests/o8/wide -D - tests/o8/wide/prog.dl > "$T/def" 2>&1; timeout 20 "$BIN" -j 1 -F tests/o8/wide -D - tests/o8/wide/prog.dl > "$T/j1" 2>&1
cmp -s "$T/def" "$T/j1" && echo "O8 default-vs-j1 equal" || { echo "O8 default-vs-j1 DIFFERS"; rc=1; }
echo "O8 programs=$n failing=$([ $rc = 0 ] && echo 0 || echo some)"
exit $rc
