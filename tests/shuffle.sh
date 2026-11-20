#!/bin/sh
# O7 helpers. shuffle.sh seed < program.dl > shuffled.dl : directives (.decl .type .input .output lines) keep their order and
# come first; the clauses (rules and program facts, one per `.` terminator outside strings) follow in a random order that
# `shuf --random-source` fixes by the seed. Line markers (# lines) are dropped.
# shuffle.sh seed -f FILE : the lines of one facts file in random order.
set -u
seed=$1
T=$(mktemp -d "${TMPDIR:-/tmp}/shuffle.XXXXXX")
trap 'rm -rf "$T"' EXIT
i=0; while [ $i -lt 512 ]; do printf 'seed %s round %s\n' "$seed" "$i" | sha256sum; i=$((i+1)); done > "$T/src"
if [ "${2:-}" = "-f" ]; then shuf --random-source="$T/src" "$3"; exit 0; fi
cat > "$T/in"
: > "$T/dirs"
grep -E '^[ 	]*\.(decl|type|input|output)([ 	(]|$)' "$T/in" > "$T/dirs"
grep -v -E '^[ 	]*(#|\.(decl|type|input|output)([ 	(]|$))' "$T/in" | awk '
  { buf = buf $0 "\n" }
  END { n = length(buf); instr = 0; cur = ""
    for (i = 1; i <= n; i++) { c = substr(buf, i, 1); if (c == "\"") instr = !instr; cur = cur c
      if (c == "." && !instr) { nx = substr(buf, i + 1, 1)
        if (nx == "\n" || nx == " " || nx == "\t" || nx == "") { gsub(/\n/, " ", cur); print cur; cur = "" } } } }' > "$T/cl"
cat "$T/dirs"
shuf --random-source="$T/src" "$T/cl"
