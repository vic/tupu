#!/bin/sh
# O4 (S90 3): width. The 17 U64 shapes of tests/d0/cases/expected-u64.txt on arith.dl, with typeRange(U64, 0, 2^64-1) and
# typeWidth(U64, 64). Souffle cannot run them (64-bit signed numbers); tupu is exact. tests/o4/expected.txt is the u64 lines of
# expected-u64.txt without their comments, written before the run. usage: BIN=build/tupu sh tests/o4.sh
set -u
BIN=${BIN:?BIN is not set}
D=tests/d0
W=$(mktemp -d "${TMPDIR:-/tmp}/o4.XXXXXX"); trap 'rm -rf "$W"' EXIT
mcpp -e utf8 -W0 $D/arith.dl > "$W/prog.dl" 2> /dev/null || { echo "O4 mcpp failed"; exit 2; }
INPUTS=$(sed -n 's/^\.input //p' $D/schema.dl | tr -d ',' | tr '\n' ' ')
MAX=18446744073709551615
new() { d=$W/$1; mkdir -p "$d"; for f in $INPUTS; do : > "$d/$f.facts"; done; C=$d; printf 'R\n' > "$d/scope.facts"
  printf 'U64\t0\t%s\nU32\t0\t4294967295\n' $MAX > "$d/typeRange.facts"; printf 'U64\t64\nU32\t32\n' > "$d/typeWidth.facts"; }
f() { r=$1; shift; for row in "$@"; do printf '%s\n' "$row" | tr ' ' '\t' >> "$C/$r.facts"; done; }
V() { f value "$1"; f valueType "$1 $2"; [ $# -ge 3 ] && f lit "$1 $3"; return 0; }
new addOver; V x U64; V k1 U64 1; V a U64; f addOp 'addO a x k1 R'
new addGuard; V x U64; V km U64 $MAX; V k1 U64 1; V a U64; f baseBelow 'x km R'; f addOp 'addO a x k1 R'
new addChainBad; V x U64; V kb U64 18446744073709551560; V k50 U64 50; V k10 U64 10; V a U64; V b U64
f baseBelow 'x kb R'; f addOp 'addA a x k50 R' 'addB b a k10 R'
idx() { new "$1"; V i U64; V n U32 5; V k U64 "$2"; f baseBelow 'i k R'; f indexRead 'rd i n R'; }
idx guard2p32 4294967296; idx guard2p64 $MAX; idx guard4 4
new subFree; V x U64; V y U64; V a U64; f subtractOp 'subO a x y R'
new subOrder; V x U64; V y U64; V a U64; f baseLeq 'y x R'; f subtractOp 'subO a x y R'
new mulOver; V x U64; V y U64; V a U64; V k U64 4294967297; f baseBelow 'x k R' 'y k R'; f multiplyOp 'mulO a x y R'
new mulOk; V x U64; V y U64; V a U64; V k U64 4294967296; f baseBelow 'x k R' 'y k R'; f multiplyOp 'mulO a x y R'
new divZero; V x U64; V y U64; V a U64; f quotientOp 'divO a x y R'
new divPos; V x U64; V y U64; V a U64; V k1 U64 1; f baseLeq 'k1 y R'; f quotientOp 'divO a x y R'
sh() { new "$1"; V x U64; V y U64; V a U64; V kw U64 "$2"; f baseBelow 'y kw R'; f shiftOp 'shO a x y R'; }
sh shiftWide 65; sh shiftOk 64
new litHigh; V k U64 18446744073709551616
new litMax; V k U64 $MAX
new remIndex; V x U64; V n U64 10; V a U64; f remainderOp 'remO a x n R'; f indexRead 'rd a n R'
: > "$W/out"
for c in addOver addGuard addChainBad guard2p32 guard2p64 guard4 subFree subOrder mulOver mulOk divZero divPos shiftWide shiftOk litHigh litMax remIndex; do
  rows=$(timeout 1 "$BIN" -F "$W/$c" -D - "$W/prog.dl" 2>&1 | awk '/^relation /{on=($2=="reject");next} on{print}' | sort | tr '\t\n' '/ ' | sed 's/ *$//')
  printf 'u64 %s : %s\n' "$c" "$rows" | sed 's/ : $/ :/' >> "$W/out"
done
diff "$W/out" tests/o4/expected.txt
echo "O4 shapes=$(wc -l < "$W/out") expected=$(wc -l < tests/o4/expected.txt) difflines=$(diff "$W/out" tests/o4/expected.txt | grep -c '^[<>]')"
! diff -q "$W/out" tests/o4/expected.txt > /dev/null && exit 1; exit 0
