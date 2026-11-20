#!/bin/sh
# bench BIN PROGRAM.dl FACTS_DIR: 3 runs of release BIN; per run wall (s), instructions, peak RSS (KiB), minor and
# major page faults; prints the median of each. An instruction count below 100000 means an inert bench: exit 3.
set -u
BIN=$1; PROG=$2; FACTS=$3
T=${TMPDIR:-/tmp}/tupu-bench.$$; mkdir -p "$T"
i=1
while [ $i -le 3 ]; do
  perf stat -x, -e instructions:u -o "$T/perf.$i" -- /usr/bin/env time -f '%e %M %R %F' -o "$T/time.$i" \
    timeout 120 "$BIN" -F "$FACTS" -D /dev/null < "$PROG" > /dev/null 2>&1
  i=$((i+1))
done
med() { sort -n | sed -n 2p; }
ins=$(for i in 1 2 3; do grep -E 'instructions' "$T/perf.$i" | cut -d, -f1; done | med)
col() { for i in 1 2 3; do tail -1 "$T/time.$i" | cut -d' ' -f"$1"; done | med; }
echo "wall_s=$(col 1) instructions=${ins:-0} peak_rss_kib=$(col 2) minor_faults=$(col 3) major_faults=$(col 4)"
rm -rf "$T"
if [ "${ins:-0}" -lt 100000 ]; then echo "INERT BENCH: instruction counter ${ins:-0}" >&2; exit 3; fi
