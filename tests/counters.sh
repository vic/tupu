#!/bin/sh
# The counters of the tc case, derived by hand in tests/counters/README (rounds, derivations, new rows, index probes),
# against the counters line of a build with -DTUPU_COUNTERS. A mutant that re-derives (full range instead of delta)
# changes `derivations` while the rows stay equal. usage: BIN=build/tupu-count sh tests/counters.sh
set -u
BIN=${BIN:?BIN is not set}
got=$(timeout 1 "$BIN" -F tests/c0/tc -D /dev/null tests/c0/tc/prog.dl 2>&1 >/dev/null | grep '^counters')
if [ "$got" = "$(cat tests/counters/tc.expected)" ]; then echo "counters tc ok [$got]"; else echo "counters tc FAIL got=[$got] expected=[$(cat tests/counters/tc.expected)]"; exit 1; fi
