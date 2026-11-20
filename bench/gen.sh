#!/bin/sh
# Generates the large bench inputs under bench/data (gitignored, never committed). Fixed seed: the same bytes every run.
# usage: sh bench/gen.sh   (writes bench/data/<name>/<relation>.facts, tab-separated, as every engine reads them)
set -eu
D=bench/data
rm -rf "$D"; mkdir -p "$D/tc" "$D/andersen" "$D/rsg"
# tc: a random graph, N nodes, M edges (the closure is the heavy part)
awk 'BEGIN { srand(1); N = 300; M = 600; for (i = 0; i < M; i++) printf "n%d\tn%d\n", int(rand() * N), int(rand() * N) }' > "$D/tc/base.facts"
# andersen: V variables; addressOf, assign, load, store facts in the ratio of a small program
awk 'BEGIN { srand(2); V = 1500
  for (i = 0; i < V / 2; i++) printf "v%d\tv%d\n", int(rand() * V), int(rand() * V) > "'$D'/andersen/AddressOf.facts"
  for (i = 0; i < V; i++) printf "v%d\tv%d\n", int(rand() * V), int(rand() * V) > "'$D'/andersen/Assign.facts"
  for (i = 0; i < V / 4; i++) printf "v%d\tv%d\n", int(rand() * V), int(rand() * V) > "'$D'/andersen/Load.facts"
  for (i = 0; i < V / 4; i++) printf "v%d\tv%d\n", int(rand() * V), int(rand() * V) > "'$D'/andersen/Store.facts" }'
# rsg: a forest of depth levels (UP, DOWN, FLAT)
awk 'BEGIN { srand(3); N = 15000
  for (i = 1; i < N; i++) { p = int(rand() * i); printf "n%d\tn%d\n", i, p > "'$D'/rsg/UP.facts"; printf "n%d\tn%d\n", p, i > "'$D'/rsg/DOWN.facts" }
  for (i = 0; i < N; i++) printf "n%d\tn%d\n", int(rand() * N), int(rand() * N) > "'$D'/rsg/FLAT.facts" }'
echo "bench data written: $(cat $D/*/*.facts | wc -l) rows"
