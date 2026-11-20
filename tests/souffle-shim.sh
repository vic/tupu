#!/bin/sh
# Stands in for the souffle command of tests/d0/cases/run.sh: it runs mcpp (the flags Souffle uses) and tupu, and prints
# the stdout form of Souffle (`---------------`, relation name, `===============`, column names, rows, `===============`).
# Only the command lines that run.sh builds are understood: [-MNAME]... [-F DIR] [-D -] FILE.dl
# TUPU is the binary; the order of the relations is the order of tupu (declaration order).
set -u
TUPU=${TUPU:?TUPU is not set}
defs=""; facts=.; file=""
while [ $# -gt 0 ]; do
  case $1 in
    -M*) for w in ${1#-M}; do defs="$defs -D$w"; done ;;
    -F) shift; facts=$1 ;;
    -D) shift ;;
    *) file=$1 ;;
  esac
  shift
done
T=$(mktemp -d "${TMPDIR:-/tmp}/shim.XXXXXX")
trap 'rm -rf "$T"' EXIT
# shellcheck disable=SC2086
mcpp -e utf8 -W0 $defs "$file" > "$T/pp.dl" 2> "$T/mcpp.err" || { echo "Error: mcpp failed: $(head -1 "$T/mcpp.err")"; exit 1; }
timeout 20 "$TUPU" -F "$facts" -D - "$T/pp.dl" > "$T/out" 2> "$T/err"; st=$?
if [ $st -ne 0 ]; then echo "Error: $(head -1 "$T/err")"; exit 1; fi
awk -v ppfile="$T/pp.dl" '
  BEGIN {
    while ((getline line < ppfile) > 0) {
      if (line ~ /^\.decl[ \t]/) {
        s = line; sub(/^\.decl[ \t]+/, "", s)
        name = s; sub(/[ \t]*\(.*/, "", name)
        cols = s; sub(/^[^(]*\(/, "", cols); sub(/\).*$/, "", cols)
        n = split(cols, parts, ",")
        h = ""
        for (i = 1; i <= n; i++) { c = parts[i]; sub(/^[ \t]+/, "", c); sub(/[ \t]*:.*$/, "", c); h = h (i > 1 ? "\t" : "") c }
        header[name] = h
      }
    }
  }
  /^relation / {
    if (open) print "==============="
    print "---------------"; print $2; print "==============="
    open = 1
    next
  }
  { print }
  END { if (open) print "===============" }
' "$T/out"
