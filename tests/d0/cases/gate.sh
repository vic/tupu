#!/bin/sh
# The whole gate: every family in fixed order, one run.sh call each, output concatenated (plumbing, ruling S31).
# Usage: SOUFFLE=... nix-shell -p mcpp --run 'sh gate.sh 2>/dev/null | cut -c1-230 | diff - expected.txt'
for fam in bounds memory arith contracts flow capabilities names tests types stack refuse corpus; do sh run.sh "$fam"; done
