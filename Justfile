# tupu recipes (rulings S79, S82, S87). Run inside `nix develop` (flake.nix). Callers run every command under timeout.
safe := "-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks"
warn := "-std=c11 -Wall -Wextra -Werror -pedantic"
src := "src/*.c"

default: build

# release build, gcc
build:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG {{safe}} -o build/tupu {{src}} -lpthread

clang:
    mkdir -p build
    clang {{warn}} -O2 -DNDEBUG {{safe}} -o build/tupu-clang {{src}} -lpthread

san:
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -o build/tupu-san {{src}} -lpthread

# release build with the bench counters printed on stderr (tests/counters.sh, the inert-bench check)
count:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG -DTUPU_COUNTERS {{safe}} -o build/tupu-count {{src}} -lpthread

# one engine mutant (O5, O6): build/tupu-mut-NAME is the release build with -DMUTANT_NAME
mutant name:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG -DTUPU_COUNTERS -DMUTANT_{{name}} {{safe}} -o build/tupu-mut-{{name}} {{src}} -lpthread

# unit tests of the library modules (everything except main.c)
unit:
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -o build/unit tests/unit.c $(ls src/*.c | grep -v '/main.c$') -lpthread
    timeout 10 build/unit

# every oracle: own cases on the release and san builds, unit, counters, O1, O2, O3, O6 (-P replay), O5 mutants, O7 shuffle.
# A failure never stops the run; the exit status is 1 when any line failed.
test: build san unit count
    #!/bin/sh
    rc=0
    for b in tupu tupu-san; do BIN=build/$b sh tests/run.sh || rc=1; done
    BIN=build/tupu-count sh tests/counters.sh || rc=1
    BIN=build/tupu sh tests/o1.sh || rc=1
    BIN=build/tupu sh tests/o2.sh || rc=1
    BIN=build/tupu sh tests/o3.sh || rc=1
    PROVFLAG=1 BIN=build/tupu sh tests/o1.sh || rc=1
    sh tests/o5.sh || rc=1
    BIN=build/tupu sh tests/o7.sh || rc=1
    exit $rc

# bench: every row of bench/set.tsv (name, program, facts dir) through hyperfine; rows are appended to bench/out/rows.txt
bench: build
    #!/bin/sh
    rm -f bench/out/rows.txt
    rc=0
    while IFS="$(printf '\t')" read -r name prog facts; do sh bench/bench.sh "$name" "$prog" "$facts" || rc=1; done < bench/set.tsv
    exit $rc

clean:
    rm -rf build bench/out
