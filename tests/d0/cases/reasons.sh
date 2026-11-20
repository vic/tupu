#!/bin/sh
# One reason line per rejected case (the NegTest reason of the later rungs): the derivation of `reject` (the input facts that
# made the obligation owed, and the negated `cleared`) and the place of the clearing rule that did not fire.
# Usage: SOUFFLE=... nix-shell -p mcpp --run 'sh reasons.sh > reasons.txt'
for fam in bounds memory arith contracts flow capabilities names tests types stack corpus; do WHY=1 sh run.sh "$fam" 2>/dev/null; done
