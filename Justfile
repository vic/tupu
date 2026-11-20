# tupu recipes (rulings S79, S82). Run inside `nix-shell` (shell.nix). Callers run every command under timeout.
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

# unit tests of the library modules (everything except main.c)
unit:
    mkdir -p build
    gcc {{warn}} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all {{safe}} -o build/unit tests/unit.c $(ls src/*.c | grep -v '/main.c$') -lpthread
    timeout 10 build/unit

# every case on the release build and the san build; a failure never stops the run
test: build san unit
    #!/bin/sh
    rc=0
    for b in tupu tupu-san; do BIN=build/$b sh tests/run.sh || rc=1; done
    exit $rc

# bench PROG=file.dl FACTS=dir
bench prog facts: build
    sh bench/bench.sh build/tupu {{prog}} {{facts}}

clean:
    rm -rf build
