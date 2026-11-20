#!/bin/sh
# Oracle runner. Each tests/c0/NAME has prog.dl, *.facts and either expected.out (stdout rows of a complete run)
# or expected.code (a rejection: exit 1, stderr "rejected CODE FILE:LINE: TEXT"; exit 2 for usage).
# BIN is the binary under test. 1 s per case (AGENTS 9); a failure never stops the run.
set -u
BIN=${BIN:?BIN is not set}
E=${TMPDIR:-/tmp}/tupu.err.$$
pass=0; fail=0
for d in tests/c0/*/; do
  name=$(basename "$d")
  extra=$(cat "$d/args" 2>/dev/null || true)
  out=$(timeout 1 "$BIN" -F "$d" -D - $extra "$d/prog.dl" 2> "$E"); st=$?
  err=$(cat "$E")
  if [ -f "$d/expected.out" ]; then
    if [ "$st" = 0 ] && [ "$out" = "$(cat "$d/expected.out")" ]; then r=ok; else r=FAIL; fi
  else
    code=$(printf '%s' "$err" | cut -d' ' -f2)
    if [ "$st" = 1 ] && [ -z "$out" ] && [ "$code" = "$(cat "$d/expected.code")" ]; then r=ok; else r=FAIL; fi
  fi
  [ $r = ok ] && pass=$((pass+1)) || fail=$((fail+1))
  echo "$BIN $name $r (exit $st) stdout_lines=$(printf '%s' "$out" | wc -l) stderr=[$err]"
done
rm -f "$E"
echo "$BIN pass=$pass fail=$fail"
[ "$fail" = 0 ]
