#!/bin/sh
# O1: the vendored public Souffle tests (tests/o1/SOURCE). For each program: mcpp, tupu with the facts dir of the test
# (or an empty dir), then every expected <relation>.csv of the test dir is compared as a sorted set with tupu's output.
# usage: BIN=build/tupu sh tests/o1.sh
set -u
BIN=${BIN:?BIN is not set}
T=$(mktemp -d "${TMPDIR:-/tmp}/o1.XXXXXX"); trap 'rm -rf "$T"' EXIT
rc=0
for d in tests/o1/evaluation/* tests/o1/example/*; do
  name=$(basename "$d")
  [ "$name" = dominance ] && continue # an O2 case
  prog=$d/$name.dl
  [ -f "$prog" ] || prog=$d/$(ls "$d" | grep '\.dl$' | grep -v edb | head -1)
  facts=$d/facts; [ -d "$facts" ] || { facts=$T/empty; mkdir -p "$facts"; }
  rm -rf "$T/out"; mkdir -p "$T/out"
  mcpp -e utf8 -W0 "$prog" 2> /dev/null | timeout 5 "$BIN" -F "$facts" -D "$T/out" - 2> "$T/err"; st=$?
  if [ $st != 0 ]; then echo "O1 $name REFUSED exit=$st $(head -c 160 "$T/err")"; rc=1; continue; fi
  ok=0; bad=0; rows=0
  for csv in "$d"/*.csv; do
    rel=$(basename "$csv" .csv)
    [ -f "$T/out/$rel.csv" ] || continue # an input relation or an internal one
    sort "$csv" > "$T/exp"; sort "$T/out/$rel.csv" > "$T/got"
    if cmp -s "$T/exp" "$T/got"; then ok=$((ok+1)); rows=$((rows+$(wc -l < "$T/got"))); else bad=$((bad+1)); echo "O1 $name $rel DIFFERS: $(diff "$T/exp" "$T/got" | head -3 | tr '\n' ' ')"; fi
  done
  echo "O1 $name relations_equal=$ok differ=$bad rows=$rows"
  [ $bad = 0 ] || rc=1
done
exit $rc
