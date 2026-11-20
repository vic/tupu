#!/bin/sh
# Compares the stream of the gate (stdin) with expected.txt and expected-souffle64.txt (ruling S85): the lines of
# expected-souffle64.txt are checked in their own file, every other line against expected.txt, each in gate order.
# Prints nothing when both equal. Usage: sh gate.sh | sh check.sh   (gate.sh already cuts the lines to 230 bytes)
D=$(dirname "$0")
A=$(mktemp); B=$(mktemp); C=$(mktemp)
trap 'rm -f "${A:?}" "${B:?}" "${C:?}"' EXIT
cat > "$A"
grep -v '^#' "$D/expected-souffle64.txt" > "$C"
grep -vxFf "$C" "$A" > "$B"
diff "$B" "$D/expected.txt"
grep -xFf "$C" "$A" | diff - "$C"
