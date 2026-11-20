#!/bin/sh
# bench BIN PROGRAM.dl FACTS_DIR: 3 runs; per run wall (s), instructions:u, cache-misses, page-faults (perf stat),
# peak RSS (KiB), minor and major faults (GNU time); prints the median of each plus load and CPU model (S79).
# Output goes to /dev/null (writing cost counts, the same for every engine). Instructions below 100000: inert, exit 3.
set -u
BIN=$1; PROG=$2; FACTS=$3
T=${TMPDIR:-/tmp}/tupu-bench.$$; mkdir -p "$T"
i=1
while [ $i -le 3 ]; do
  perf stat -x, -e instructions:u,cache-misses,page-faults -o "$T/perf.$i" -- \
    time -f '%e %M %R %F' -o "$T/time.$i" timeout 120 "$BIN" -F "$FACTS" -D - "$PROG" > /dev/null 2>&1
  i=$((i+1))
done
med() { sort -n | sed -n 2p; }
pc() { for i in 1 2 3; do grep -E "$1" "$T/perf.$i" | cut -d, -f1; done | med; }
col() { for i in 1 2 3; do tail -1 "$T/time.$i" | cut -d' ' -f"$1"; done | med; }
ins=$(pc instructions)
echo "wall_s=$(col 1) instructions=${ins:-0} cache_misses=$(pc cache-misses) perf_page_faults=$(pc page-faults) peak_rss_kib=$(col 2) minor_faults=$(col 3) major_faults=$(col 4) load=\"$(cut -d' ' -f1-3 /proc/loadavg)\" cpu=\"$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')\""
rm -rf "$T"
if [ "${ins:-0}" -lt 100000 ]; then echo "INERT BENCH: instruction counter ${ins:-0}" >&2; exit 3; fi
