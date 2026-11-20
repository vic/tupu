#!/bin/sh
# row NAME COMMAND: hyperfine (-N, 1 warmup, 3 runs; stdout discarded, the same for every engine) on COMMAND; prints one row with the
# median wall time, peak resident set, instructions and cache misses from bench/out/NAME.json, page faults from one perf stat run
# (`not measured` where perf does not work), the load and the CPU model. Rows are appended to bench/out/rows.txt.
set -u
NAME=$1; CMD=$2
OUT=bench/out; mkdir -p "$OUT"
# S98: a run counts only while the 1-min load average is below 4 at its start and at its end; otherwise it is discarded and repeated
load1() { cut -d" " -f1 /proc/loadavg; }
low() { awk -v l="$1" 'BEGIN { exit !(l < 4) }'; }
tries=0
while :; do
  L0=$(load1)
  if low "$L0"; then
    hyperfine -N --warmup 1 --runs "${RUNS:-3}" --export-json "$OUT/$NAME.json" "$CMD" > /dev/null 2>&1 || { echo "row $NAME: hyperfine failed: $CMD" | tee -a "$OUT/rows.txt"; exit 2; }
    L1=$(load1); low "$L1" && break
  fi
  tries=$((tries + 1)); [ "$tries" -ge "${MAXTRIES:-1000}" ] && { echo "row $NAME: load never below 4 (last $L0)" | tee -a "$OUT/rows.txt"; exit 3; }
  sleep 20
done
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
echo "row $NAME wall_s=${wall:-?} peak_rss_bytes=${rss:-?} instructions=${ins:-not measured} cache_misses=${cm:-not measured} page_faults=$pf load_start=$L0 load_end=$L1 cpu=\"$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')\"" | tee -a "$OUT/rows.txt"
