# tupu recipes (rulings S79, S82, S87). Run inside `nix develop` (flake.nix). Callers run every command under timeout.
safe := "-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks"
warn := "-std=c11 -Wall -Wextra -Werror -pedantic"
src := "src/*.c"

default: build

# release build, gcc
build:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG {{safe}} -o build/tupu {{src}} -pthread

clang:
    mkdir -p build
    clang {{warn}} -O2 -DNDEBUG {{safe}} -o build/tupu-clang {{src}} -pthread

san:
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -o build/tupu-san {{src}} -pthread

# release build with the bench counters printed on stderr (tests/counters.sh, the inert-bench check)
count:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG -DTUPU_COUNTERS {{safe}} -o build/tupu-count {{src}} -pthread

# one engine mutant (O5, O6): build/tupu-mut-NAME is the release build with -DMUTANT_NAME
mutant name:
    mkdir -p build
    gcc {{warn}} -O2 -DNDEBUG -DTUPU_COUNTERS -DMUTANT_{{name}} {{safe}} -o build/tupu-mut-{{name}} {{src}} -pthread

# unit tests of the library modules (everything except main.c)
unit:
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -o build/unit tests/unit.c $(ls src/*.c | grep -v '/main.c$') -pthread
    timeout 10 build/unit

# static library build (libtupu.a)
lib:
    mkdir -p build/obj
    for f in $(ls src/*.c | grep -v '/main.c$'); do gcc {{warn}} -O2 -DNDEBUG {{safe}} -Iinclude -c "$f" -o "build/obj/$(basename "$f" .c).o"; done
    ar rcs build/libtupu.a build/obj/*.o

# in-process C library API tests (ASan/UBSan and release)
api-test: lib
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -Iinclude -o build/api_test_san tests/api_test.c $(ls src/*.c | grep -v '/main.c$') -pthread
    timeout 30 build/api_test_san
    gcc {{warn}} -O2 -DNDEBUG {{safe}} -Iinclude -o build/api_test tests/api_test.c build/libtupu.a -pthread
    timeout 30 build/api_test

# every oracle on the binary of `nix build` (result/bin/tupu, built once: no test recipe compiles tupu again) and on the san build.
# A failure never stops the run; the exit status is 1 when any line failed. The O5 mutants and the counters check compile
# mutant builds on purpose: they run in `just mutants`.
test: san unit api-test
    #!/bin/sh
    rc=0
    B=result/bin/tupu
    [ -x $B ] || { echo "run nix build -o result first"; exit 1; }
    BIN=$B sh tests/run.sh || rc=1
    BIN=build/tupu-san sh tests/run.sh || rc=1
    BIN=$B sh tests/o1.sh || rc=1
    BIN=$B sh tests/o2.sh || rc=1
    BIN=$B sh tests/o3.sh || rc=1
    BIN=$B sh tests/o4.sh || rc=1
    BIN=$B sh tests/o8.sh || rc=1
    BIN=$B sh tests/lattice.sh || rc=1
    BIN=build/tupu-san sh tests/lattice.sh || rc=1
    O8_SAN=1 BIN=build/tupu-san sh tests/o8.sh || rc=1
    PROVFLAG=1 BIN=$B sh tests/o1.sh || rc=1
    [ -n "${SKIP_O7:-}" ] || BIN=$B sh tests/o7.sh || rc=1
    exit $rc

# O5 and the counters check: builds the engine mutants (compiles on purpose); the flips are printed per mutant
mutants: count
    #!/bin/sh
    rc=0
    BIN=build/tupu-count sh tests/counters.sh || rc=1
    BIN=result/bin/tupu sh tests/o5.sh || rc=1
    just mutant LATTICEDIR && BIN=result/bin/tupu MUT=build/tupu-mut-LATTICEDIR sh tests/lattice.sh || rc=1
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
