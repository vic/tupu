# souffle-compat: the command line that cases/run.sh of the D0 suite gives to `souffle`:
#   souffle [-MNAME ...] [-F DIR] [-D -] FILE.dl
# It runs mcpp (the flags Souffle uses), then tupu, and prints Souffle's stdout form: `---------------`, the relation name,
# `===============`, the rows, `===============`. tupu rejects: `Error: ` and its stderr line, exit 1.
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
T=$(mktemp -d "${TMPDIR:-/tmp}/souffle-compat.XXXXXX")
trap 'rm -rf "$T"' EXIT
# shellcheck disable=SC2086
mcpp -e utf8 -W0 $defs "$file" > "$T/pp.dl" 2> "$T/mcpp.err" || { echo "Error: mcpp failed: $(head -1 "$T/mcpp.err")"; exit 1; }
timeout 60 tupu -F "$facts" -D - "$T/pp.dl" > "$T/out" 2> "$T/err" || { echo "Error: $(head -1 "$T/err")"; exit 1; }
awk '
  /^relation / {
    if (open) print "==============="
    print "---------------"; print $2; print "==============="
    open = 1
    next
  }
  { print }
  END { if (open) print "===============" }
' "$T/out"
