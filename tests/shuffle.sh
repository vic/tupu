#!/bin/sh
# O7 helpers. shuffle.sh seed < program.dl > shuffled.dl : directive lines (.decl .type .input .output) keep their order and
# come first; the clauses (rules and program facts: text up to a line that ends in `.`) follow in a random order that
# `shuf --random-source` fixes by the seed. `#` lines and blank lines are dropped.
# shuffle.sh seed -f FILE : the lines of one facts file in random order.
set -u
seed=$1
T=$(mktemp -d "${TMPDIR:-/tmp}/shuffle.XXXXXX")
trap 'rm -rf "$T"' EXIT
rnd() { awk -v s="$seed" -v n="$1" 'BEGIN { srand(s); for (i = 0; i < n; i++) printf "%c", int(rand() * 256) }' > "$T/src"; }
if [ "${2:-}" = "-f" ]; then
  [ -s "$3" ] || { cat "$3"; exit 0; }
  rnd $(( $(wc -l < "$3") * 16 + 256 )); shuf --random-source="$T/src" "$3"; exit 0
fi
awk -v d="$T/dirs" -v c="$T/cl" '
  /^[ \t]*#/ || /^[ \t]*$/ { next }
  cur == "" && /^[ \t]*\.(decl|type|input|output)([ \t(]|$)/ { print > d; next }
  { cur = cur (cur == "" ? "" : " ") $0 }
  /\.[ \t]*$/ { print cur > c; cur = "" }
  END { if (cur != "") print cur > c }'
touch "$T/dirs" "$T/cl"
cat "$T/dirs"
rnd $(( $(wc -l < "$T/cl") * 16 + 256 ))
shuf --random-source="$T/src" "$T/cl"
