#!/bin/sh
# Compiles every bench program with the two comparators; prints the compile wall time of each (reported separately from the run).
# usage (inside nix develop, as a background job): sh bench/compile-comparators.sh   needs bench/comp/{souffle64,flowlog} (bench/comparators.nix)
set -u
S=bench/comp/souffle64/bin/souffle
F=bench/comp/flowlog/bin/flowlog-compiler
mkdir -p bench/comp/bin
for p in tc andersen rsg; do
  case "${ENGINES:-souffle flowlog}" in *souffle*) ;; *) break ;; esac
  s=$(date +%s)
  nice -n 10 timeout 900 "$S" -o "bench/comp/bin/souffle_$p" "bench/prog/$p.dl" > "bench/comp/bin/souffle_$p.log" 2>&1
  echo "compile souffle $p exit=$? wall=$(( $(date +%s) - s ))s"
done
for p in tc andersen rsg; do
  case "${ENGINES:-souffle flowlog}" in *flowlog*) ;; *) break ;; esac
  s=$(date +%s)
  FLOWLOG_RUNTIME_PATH=/home/vic/hk/FlowLog/flowlog-runtime nice -n 10 timeout 900 "$F" -o "bench/comp/bin/flowlog_$p" -F "bench/data/$p" -D - "bench/prog/$p.dl" > "bench/comp/bin/flowlog_$p.log" 2>&1
  echo "compile flowlog $p exit=$? wall=$(( $(date +%s) - s ))s"
done
