#!/bin/sh
# Oracle runner. Each tests/c0/NAME has prog.dl, *.facts and either expected.out (stdout rows of a complete run)
# or expected.code (the code of a rejection: stderr "rejected: CODE", exit 2). BIN is the binary under test.
set -u
BIN=${BIN:?BIN is not set}
pass=0; fail=0
for d in tests/c0/*/; do
  name=$(basename "$d")
  out=$(timeout 5 "$BIN" -F "$d" -D - < "$d/prog.dl" 2> "${TMPDIR:-/tmp}/tupu.err.$$"); st=$?
  err=$(cat "${TMPDIR:-/tmp}/tupu.err.$$")
  if [ -f "$d/expected.out" ]; then
    if [ "$st" = 0 ] && [ "$out" = "$(cat "$d/expected.out")" ]; then r=ok; else r=FAIL; fi
  else
    if [ "$st" = 2 ] && [ "$err" = "rejected: $(cat "$d/expected.code")" ]; then r=ok; else r=FAIL; fi
  fi
  [ $r = ok ] && pass=$((pass+1)) || fail=$((fail+1))
  echo "$BIN $name $r (exit $st) stdout_lines=$(printf '%s' "$out" | wc -l) stderr=[$err]"
done
rm -f "${TMPDIR:-/tmp}/tupu.err.$$"
echo "pass=$pass fail=$fail"
[ "$fail" = 0 ]
