#!/bin/sh
# Golden cases for the Souffle form of the stigi rules (ruling SOUFFLE FIRST; step D0 of vic/plan/impl-plan.md).
# Run from this directory, with the 64-bit Souffle of ../souffle64.nix (gate.sh runs every family in fixed order):
#   SOUFFLE=$(timeout 300 nix-build ../souffle64.nix --no-out-link)/bin/souffle timeout 280 nix-shell -p mcpp --run 'sh run.sh 2>/dev/null | cut -c1-230 | diff - expected.txt'
# Each line is: family case [mutant] : the rows of the verdict (rejected subject/code pairs, then emitter decisions).
# Compare with expected.txt: `sh run.sh | diff - expected.txt` must print nothing. Expected lines are written before the run.
set -eu
W=${TMPDIR:-/tmp}/stigi-datalog.$$
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
FAMILY=${1:-all}   # run.sh FAMILY runs one family: bounds memory arith contracts flow capabilities
want() { [ "$FAMILY" = all ] || [ "$FAMILY" = "$1" ]; }
SOUFFLE=${SOUFFLE:-souffle}
if ! command -v "$SOUFFLE" > /dev/null 2>&1; then echo "run.sh: SOUFFLE is not executable: $SOUFFLE (build it: SOUFFLE=\$(timeout 300 nix-build ../souffle64.nix --no-out-link)/bin/souffle)" >&2; exit 2; fi
# every input relation of schema.dl needs a facts file; a case fills the ones it uses
INPUTS=$(sed -n 's/^\.input //p' ../schema.dl | tr -d ',' | tr '\n' ' ')

new_case() {
  d=$W/$1; mkdir -p "$d"
  for f in $INPUTS; do : > "$d/$f.facts"; done
}
# Jobs: run and verdict queue one Souffle call each (arguments one per line in job.N); the jobs run in parallel and
# their lines are printed in queue order at the end, so the output never depends on timing.
n=0
# ONLY=glob runs only the cases whose name matches (splits a family under the command cap)
queue() { case "$3" in ${ONLY:-*}) ;; *) return 0 ;; esac; n=$((n+1)); printf "%s\n" "$@" > "$W/job.$n"; fillrows "$3"; }
# ruling S37: a case with operation rows and no row table of its own gets the generic table (every operation kind
# requires the row OperationRow and the case emits it); a case that states its own requiresRow rows is left alone
fillrows() {
  d=$W/$1; [ -d "$d" ] && [ -s "$d/operation.facts" ] && [ ! -s "$d/requiresRow.facts" ] || return 0
  awk -F"\t" '{ print $2 "\tOperationRow" }' "$d/operation.facts" | sort -u > "$d/requiresRow.facts"
  awk -F"\t" '{ print $1 "\tOperationRow" }' "$d/operation.facts" > "$d/emitted.facts"
}
# family case mutant [file]: the rows are printed in the order of the engine (families bounds and memory)
run() { queue run "$@"; }
# family case mutant file: only the reject rows, sorted; any message of Souffle is shown
verdict() { queue verdict "$@"; }
# family case mutant file relation: the rows of one output relation, sorted
relrows() { queue rel "$@"; }
cat > "$W/exec.sh" <<"EOF"
# one job: read its arguments, run Souffle, write the line to res.N
i=$1
{ read -r kind; read -r fam; read -r cas; read -r mut; read -r file; read -r rel; } < "$W/job.$i"
# the coverage and reasons modes look at the plain cases only, and coverage needs no verdict
if [ -n "${COVER:-}${WHY:-}" ] && [ -n "$mut" ]; then exit 0; fi
if [ -n "${COVER:-}" ]; then : > "$W/res.$i"; else
{
  printf "%s %s %s: " "$fam" "$cas" "$mut"
  if [ "$kind" = refuse ]; then
    $SOUFFLE "refuse/$cas.dl" -D - > "$W/out.$i" 2>&1 || true
    msg=$(grep -m1 "^Error" "$W/out.$i" | sed "s/ in file .* at line [0-9]*//")
    printf "%s " "${msg:-accepted}"
  elif [ "$kind" = rel ]; then
    $SOUFFLE ${mut:+"$mut"} -F "$W/$cas" "../${file:-$fam}.dl" > "$W/out.$i" 2> "$W/err.$i" || printf "SOUFFLE-FAILED "
    awk -v want="$rel" "/^---------------\$/ { getline name; cur=name; next } /^===============\$/ { inrows = !inrows; next } inrows && cur == want { print }" "$W/out.$i" | sort | tr "\t\n" "/ "
    tr "\n" " " < "$W/err.$i"
  elif [ "$kind" = run ]; then
    $SOUFFLE ${mut:+"$mut"} -F "$W/$cas" "../${file:-$fam}.dl" 2>&1 | grep -v -e "^===" -e "^subject" -e "^v	p" | tr "\n" " " | tr "\t" "/"
  else
    $SOUFFLE ${mut:+"$mut"} -F "$W/$cas" "../${file:-$fam}.dl" > "$W/out.$i" 2> "$W/err.$i" || printf "SOUFFLE-FAILED "
    awk "/^====/ { n++; next } n == 1 { print }" "$W/out.$i" | sort | tr "\t\n" "/ "
    tr "\n" " " < "$W/err.$i"
  fi
  echo
} > "$W/res.$i"
fi
# COVER=1: per obligation code, the cases where it is owed and cleared (accepted) or owed and not cleared (rejected)
# WHY=1: per rejected row, the derivation of `reject` and the place of its clearing rule (reasons.txt)
if [ -z "$mut" ] && [ "$kind" != refuse ] && [ "$kind" != run ] && { [ "$kind" != rel ] || [ "$rel" = reject ]; }; then
  if [ -n "${COVER:-}" ]; then
    $SOUFFLE -MCOVER -F "$W/$cas" "../${file:-$fam}.dl" 2> /dev/null | awk -v tag="$fam/$cas" '
      /^---------------$/ { getline name; cur = name; next }
      /^===============$/ { inrows = !inrows; next }
      inrows && cur == "owes" { split($0, p, "\t"); owed[p[1] SUBSEP p[2]] = 1 }
      inrows && cur == "cleared" { split($0, p, "\t"); clr[p[1] SUBSEP p[2]] = 1 }
      END { for (k in owed) { split(k, q, SUBSEP); if (k in clr) acc[q[2]] = 1; else rej[q[2]] = 1 }
            for (c in acc) print c "\taccepted\t" tag; for (c in rej) print c "\trejected\t" tag }' > "$W/cover.$i" || true
  fi
  if [ -n "${WHY:-}" ]; then
    rows=$(sed 's/^[^:]*: //' "$W/res.$i")
    for tok in $rows; do
      case "$tok" in */*) ;; *) continue ;; esac
      subj=${tok%%/*}; code=${tok#*/}
      tree=$(printf 'explain reject("%s", "%s")\nexit\n' "$subj" "$code" | $SOUFFLE -t explain -F "$W/$cas" "../${file:-$fam}.dl" 2> /dev/null | grep -E '^ *[A-Za-z]+\(' | sed 's/  */ /g; s/ $//' | tr '\n' ';' | sed 's/;/ ; /g')
      loc=$(grep -n "cleared([a-z]*, \"$code\")" ../*.dl | tail -1 | cut -d: -f1,2 | sed 's|^\.\./||')
      ltxt=""; [ -n "$loc" ] && ltxt=$(sed -n "${loc##*:}p" "../${loc%%:*}" | sed "s/  */ /g")
      echo "$fam $cas : $tok : $tree cleared only by ${loc:-the generic clearing rule over a variable code}: $ltxt" >> "$W/why.$i"
    done
  fi
fi
EOF
# Width probe: a 32-bit Souffle wraps this literal and must not be used for arithmetic cases.
cat > "$W/width.dl" <<'EOF'
.decl wide(n: number)
wide(4294967296).
.decl ok(n: number)
ok(n) :- wide(n), n > 4294967295.
.output ok(IO=stdout)
EOF
printf "width 64-bit-numbers (1 = yes, 0 = this Souffle refuses numbers of 2^31 or more and wraps its sums): " >&2
$SOUFFLE "$W/width.dl" 2>&1 | grep -c '^4294967296$' >&2 || true

# helpers for the cases of the later families: f REL 'field field ...' ... appends rows to the case D
f() { r=$1; shift; for row in "$@"; do printf '%s\n' "$row" | tr ' ' '\t' >> "$D/$r.facts"; done; }
mk() { new_case "$1"; D=$W/$1; }
# V name type [literal]: an SSA value of a model type, optionally a literal
V() { f value "$1"; f valueType "$1 $2"; if [ $# -ge 3 ]; then f lit "$1 $3"; fi; }
# the model types of the arithmetic family: exact rows. U64 does not fit Souffle's signed 64-bit numbers (gap G3):
# U32 stands for an unsigned type and I64 is the 64-bit type.
types() {
  f typeRange 'U8 0 255' 'I8 -128 127' 'U32 0 4294967295' 'I64 -9223372036854775808 9223372036854775807'
  f typeWidth 'U8 8' 'I8 8' 'U32 32' 'I64 64'
  f scope R
}

# ---- bounds: Demo.Bytes.count (vic/spec/language.md section 4) ----
# arguments: name, step of matches, step of index, guard|x, inv|x
bounds_case() {
  new_case "$1"; d=$W/$1; D=$d
  printf '%s\n' size m0 i0 m1 i1 m2 m3 i2 k1 k2 > "$d/value.facts"
  printf '%s\n' R H B T F J E > "$d/scope.facts"
  printf 'H\tR\nB\tH\nT\tB\nF\tB\nJ\tB\nE\tH\n' > "$d/parent.facts"
  printf 'm0\t0\ni0\t0\nk1\t1\nk2\t2\n' > "$d/lit.facts"
  printf '%s\n' size m0 i0 m1 i1 m2 m3 i2 > "$d/u64.facts"
  for v in size m0 i0 m1 i1 m2 m3 i2 k1 k2; do printf '%s\tByte\n' $v; done > "$d/valueType.facts"
  printf 'Byte\t0\t255\n' > "$d/typeRange.facts"
  printf 'addMatches\tm2\tm1\tk%s\tT\naddIndex\ti2\ti1\tk%s\tJ\n' "$2" "$3" > "$d/addOp.facts"
  printf 'm3\tm2\tT\tm1\tF\tJ\n' > "$d/phi.facts"
  if [ "$4" = guard ]; then printf 'i1\tsize\tB\n' > "$d/baseBelow.facts"; fi
  printf 'm1\ti1\tH\nsize\ti1\tE\n' > "$d/baseLeq.facts"
  printf 'read\ti1\tsize\tB\n' > "$d/indexRead.facts"
  printf 'entryA\tm0\ti0\tR\tinvariant-unproved\nbackA\tm3\ti2\tJ\tinvariant-unproved\nexit\tm1\tsize\tE\texit-unproved\n' > "$d/leqOwed.facts"
  if [ "$5" = inv ]; then
    printf 'i1\tsize\tH\n' >> "$d/baseLeq.facts"
    printf 'entryB\ti0\tsize\tR\tinvariant-unproved\nbackB\ti2\tsize\tJ\tinvariant-unproved\n' >> "$d/leqOwed.facts"
  fi
}
if want bounds; then
bounds_case pos 1 1 guard inv
bounds_case noinv 1 1 guard x
bounds_case step2 1 2 guard inv
bounds_case noguard 1 1 x inv
bounds_case match2 2 1 guard inv
for c in pos noinv step2 noguard match2; do run bounds $c '' arith; done
run bounds match2 -MMUTANT_MONOTONE arith
run bounds match2 -MMUTANT_PHI arith
run bounds noguard -MMUTANT_SELF arith
run bounds step2 -MMUTANT_UPPER arith
. ./axioms.cases
fi

# ---- memory: six flow shapes ----
# arguments: name, then pairs "relation rows"
memory_case() {
  new_case "$1"; d=$W/$1; shift
  while [ $# -gt 0 ]; do printf "$2" > "$d/$1.facts"; shift 2; done
}
if want memory; then
memory_case heapOnce entry 'p1\n' next 'p1\tp2\np2\tp3\np3\tp4\n' exit 'p4\n' heap 'h\n' alloc 'a\th\tp1\n' use 'p2\ta\n'
memory_case arenaPhase entry 'p1\n' next 'p1\tp2\np2\tp3\np3\tp4\n' exit 'p4\n' arena 'r\n' alloc 'a\tr\tp1\nb\tr\tp2\n' use 'p3\ta\np4\tb\n'
memory_case arenaGarbage entry 'p1\n' next 'p1\tp2\np2\tp3\np3\tp4\n' exit 'p4\n' arena 'r\n' alloc 'a\tr\tp1\nb\tr\tp3\n' use 'p2\ta\np4\tb\n'
memory_case arenaLoop entry 'h\n' next 'h\tp1\np1\tp2\np2\th\nh\tx\n' exit 'x\n' arena 'r\n' alloc 'a\tr\tp1\n' use 'p2\ta\n'
memory_case arenaLoopKept contentRead 'x\tt\n' entry 'p0\n' next 'p0\th\nh\tp1\np1\tp2\np2\th\nh\tx\n' exit 'x\n' arena 'r\n' alloc 't\tr\tp0\na\tr\tp1\n' move 'p2\ta\tt\n' use 'x\tt\n'
memory_case heapLoop entry 'h\n' next 'h\tp1\np1\tp2\np2\th\nh\tx\n' exit 'x\n' heap 'g\n' alloc 'a\tg\tp1\n' use 'p2\ta\n'
memory_case heapUnused entry 'p1\n' next 'p1\tp2\n' exit 'p2\n' heap 'h\n' alloc 'a\th\tp1\n'
for c in heapOnce arenaPhase arenaGarbage arenaLoop arenaLoopKept heapLoop heapUnused; do run memory $c ''; done
run memory arenaLoop -MMUTANT_NOKILL
. ./memory2.cases
fi

# ---- arithmetic (vic/spec/rules.md 3): cases in arith.cases, one line each ----
want arith && . ./arith.cases
want contracts && . ./contracts.cases
want flow && . ./flow.cases
want capabilities && . ./capabilities.cases
want names && . ./names.cases
want tests && . ./tests.cases
want types && . ./types.cases
want stack && . ./stack.cases
want refuse && . ./refuse.cases
want corpus && . ./corpus.cases

# run every queued job (10 at a time) and print the results in queue order
export W SOUFFLE
seq 1 "$n" | xargs -P 12 -I{} sh "$W/exec.sh" {}
i=1
while [ "$i" -le "$n" ]; do
  if [ -n "${COVER:-}" ]; then cat "$W/cover.$i" 2>/dev/null || true
  elif [ -n "${WHY:-}" ]; then cat "$W/why.$i" 2>/dev/null || true
  else cat "$W/res.$i"; fi
  i=$((i+1))
done
