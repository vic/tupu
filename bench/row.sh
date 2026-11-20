#!/bin/sh
# row NAME COMMAND: hyperfine (-N, 1 warmup, 3 runs; stdout discarded, the same for every engine) on COMMAND; prints one row with the
# median wall time, peak resident set, instructions and cache misses from bench/out/NAME.json, page faults from one perf stat run
# (`not measured` where perf does not work), the load and the CPU model. Rows are appended to bench/out/rows.txt.
set -u
NAME=$1; CMD=$2
OUT=bench/out; mkdir -p "$OUT"
hyperfine -N --warmup 1 --runs "${RUNS:-3}" --export-json "$OUT/$NAME.json" "$CMD" > /dev/null 2>&1 || { echo "row $NAME: hyperfine failed: $CMD" | tee -a "$OUT/rows.txt"; exit 2; }
med() { awk -v key="$1" '
  /"summary":/ { s = 1 }
  s && $0 ~ "\"" key "\": *\\{" { cur = 1; next }
  cur && /"median":/ { gsub(/[^0-9.eE+-]/, "", $2); print $2; exit }' "$OUT/$NAME.json"; }
wall=$(med time_wall_clock); rss=$(med memory_peak_resident); ins=$(med instructions); cm=$(med cache_misses)
pf="not measured"
if perf stat -e page-faults -x, -o "$OUT/$NAME.pf" -- $CMD > /dev/null 2>&1; then
  n=$(grep -m1 page-faults "$OUT/$NAME.pf" | cut -d, -f1); case $n in ''|*[!0-9]*) ;; *) pf=$n ;; esac
fi
rm -f "$OUT/$NAME.pf"
echo "row $NAME wall_s=${wall:-?} peak_rss_bytes=${rss:-?} instructions=${ins:-not measured} cache_misses=${cm:-not measured} page_faults=$pf load=\"$(cut -d' ' -f1-3 /proc/loadavg)\" cpu=\"$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')\"" | tee -a "$OUT/rows.txt"
