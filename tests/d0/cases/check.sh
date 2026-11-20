#!/bin/sh
# Compares the stream of the gate (stdin) with expected.txt and expected-exact.txt (rulings S85, S91): the lines of
# expected-exact.txt (the verdicts of an exact engine where Souffle wrapped at 64 bits) are checked in their own file, every
# other line against expected.txt, each in gate order. expected-souffle64.txt is a record and is not read.
# Prints nothing when both equal. Usage: sh gate.sh | sh check.sh   (gate.sh already cuts the lines to 230 bytes)
D=$(dirname "$0")
A=$(mktemp); B=$(mktemp); C=$(mktemp)
trap 'rm -f "${A:?}" "${B:?}" "${C:?}"' EXIT
cat > "$A"
grep -v '^#' "$D/expected-exact.txt" > "$C"
grep -vxFf "$C" "$A" > "$B"
diff "$B" "$D/expected.txt"
grep -xFf "$C" "$A" | diff - "$C"
