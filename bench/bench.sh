#!/bin/sh
# bench NAME PROGRAM.dl FACTS_DIR [BIN]: hyperfine (-N, 1 warmup, 5 runs) on the release build; the row takes the median of
# wall time, peak resident set, instructions and cache misses from bench/out/NAME.json. Page faults come from one extra
# `perf stat` run; where perf does not work the row says `page-faults: not measured`. Each row keeps load and CPU model.
# An instruction count below 100000 is an inert bench: the row is refused (exit 3).
set -u
NAME=$1; PROG=$2; FACTS=$3; BIN=${4:-build/tupu}
OUT=bench/out; mkdir -p "$OUT"
mcpp -e utf8 -W0 "$PROG" > "$OUT/$NAME.dl" 2> /dev/null || { echo "bench $NAME: mcpp failed" >&2; exit 2; }
hyperfine -N --warmup 1 --runs 5 --export-json "$OUT/$NAME.json" "$BIN -F $FACTS -D - $OUT/$NAME.dl" > /dev/null 2>&1 \
  || { echo "bench $NAME: hyperfine failed" >&2; exit 2; }
med() { awk -v key="$1" '
  $0 ~ "\"" key "\": *\\{" { cur = 1; next }
  cur && /"median":/ { gsub(/[^0-9.eE+-]/, "", $2); print $2; exit }' "$OUT/$NAME.json"; }
wall=$(med time_wall_clock); rss=$(med memory_peak_resident); ins=$(med instructions); cm=$(med cache_misses)
pf="page-faults: not measured"
if perf stat -e page-faults -x, -o "$OUT/$NAME.pf" -- "$BIN" -F "$FACTS" -D - "$OUT/$NAME.dl" > /dev/null 2>&1; then
  n=$(grep -m1 page-faults "$OUT/$NAME.pf" | cut -d, -f1)
  case $n in ''|*[!0-9]*) ;; *) pf="page-faults: $n" ;; esac
fi
rm -f "$OUT/$NAME.pf"
echo "bench $NAME wall_s=${wall:-?} peak_rss_bytes=${rss:-?} instructions=${ins:-?} cache_misses=${cm:-?} $pf load=\"$(cut -d' ' -f1-3 /proc/loadavg)\" cpu=\"$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')\"" | tee -a "$OUT/rows.txt"
case ${ins:-0} in ''|*[!0-9]*) ins_int=0 ;; *) ins_int=${ins%%.*} ;; esac
if [ "${ins_int:-0}" -lt 100000 ]; then echo "INERT BENCH: instructions ${ins:-?} below 100000" >&2; exit 3; fi
