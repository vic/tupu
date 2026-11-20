#!/bin/sh
# Step 4 (S94): every row of the table: tupu -j 1 and -j 6, Souffle compiled -j 1 and -j 6, FlowLog -w 1 and -w 6, on the same program and
# the same facts (bench/gen.sh). tupu is an interpreter: Souffle also runs in its interpreter mode (rows souffle-interp), and the compiled Souffle and FlowLog rows exclude their compile time (bench/compile-comparators.sh prints it separately). Needs the builds of bench/comparators.nix (bench/comp/...) and bench/compile-comparators.sh.
# usage: sh bench/compare.sh [program...]   (inside nix develop; run as a background job, every row is one short command)
set -u
progs=${*:-tc andersen rsg}
T=${TUPU:-result/bin/tupu}
mkdir -p bench/out/pp
: > bench/out/rows.txt
for p in $progs; do
  mcpp -e utf8 -W0 "bench/prog/$p.dl" > "bench/out/pp/$p.dl" 2> /dev/null
  d=bench/data/$p
  for j in 1 6; do
    sh bench/row.sh "$p-tupu-j$j" "$T -b 1000000000 -j $j -F $d -D - bench/out/pp/$p.dl"
    sh bench/row.sh "$p-souffle-interp-j$j" "bench/comp/souffle64/bin/souffle -j $j -F $d -D - bench/out/pp/$p.dl"
    [ -x "bench/comp/bin/souffle_$p" ] && sh bench/row.sh "$p-souffle-j$j" "bench/comp/bin/souffle_$p -j $j -F $d -D -"
    [ -x "bench/comp/bin/flowlog_$p" ] && sh bench/row.sh "$p-flowlog-w$j" "bench/comp/bin/flowlog_$p -w $j -F $d -D -"
  done
done
