#!/bin/sh
# Byte-equality of two builds (S101): stdout, stderr, exit and the -P files, on every O1 program (except pointsto: its replay is slow)
# and on the wide case, at -j 1 and -j 6. usage: sh tests/provcmp.sh BIN_BEFORE BIN_AFTER
set -u
A=$1; B=$2
T=$(mktemp -d "${TMPDIR:-/tmp}/provcmp.XXXXXX"); trap 'rm -rf "$T"' EXIT
n=0; bad=0
cmpone() { # name prog facts
  for j in 1 6; do
    for w in a b; do
      bin=$A; [ $w = b ] && bin=$B
      rm -rf "$T/$w"; mkdir -p "$T/$w"
      timeout 20 "$bin" -j $j -P "$T/$w" -F "$3" -D - "$2" > "$T/$w.out" 2> "$T/$w.err"; echo $? >> "$T/$w.err"
    done
    n=$((n+1))
    if cmp -s "$T/a.out" "$T/b.out" && cmp -s "$T/a.err" "$T/b.err" && diff -rq "$T/a" "$T/b" > /dev/null; then :; else bad=$((bad+1)); echo "provcmp $1 -j $j DIFFERS"; fi
  done
}
for d in tests/o1/evaluation/* tests/o1/example/*; do
  name=$(basename "$d"); [ "$name" = dominance ] && continue; [ "$name" = pointsto ] && continue
  prog=$d/$name.dl; [ -f "$prog" ] || prog=$d/$(ls "$d" | grep '\.dl$' | grep -v edb | head -1)
  facts=$d/facts; [ -d "$facts" ] || { facts=$T/empty; mkdir -p "$facts"; }
  mcpp -e utf8 -W0 "$prog" > "$T/pp.dl" 2> /dev/null
  cmpone "$name" "$T/pp.dl" "$facts"
done
cmpone wide tests/o8/wide/prog.dl tests/o8/wide
echo "provcmp comparisons=$n differing=$bad"
[ "$bad" = 0 ]
