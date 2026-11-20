# tupu build (ruling S79). Run inside `nix-shell` (shell.nix). Callers run every command under timeout.
CC = gcc
SAFE = -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
WARN = -std=c11 -Wall -Wextra -Werror -pedantic
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

all: build/tupu
clang: build/tupu-clang
san: build/tupu-san

build/tupu: $(SRC) $(HDR)
	@mkdir -p build
	gcc $(WARN) -O2 -DNDEBUG $(SAFE) -o $@ $(SRC)

build/tupu-clang: $(SRC) $(HDR)
	@mkdir -p build
	clang $(WARN) -O2 -DNDEBUG $(SAFE) -o $@ $(SRC)

build/tupu-san: $(SRC) $(HDR)
	@mkdir -p build
	gcc $(WARN) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(SAFE) -o $@ $(SRC)

# every case runs on the release, the clang and the san build; a failure never stops the run
test: build/tupu build/tupu-clang build/tupu-san
	@rc=0; for b in tupu tupu-clang tupu-san; do BIN=build/$$b sh tests/run.sh || rc=1; done; exit $$rc

# bench PROG=file.dl FACTS=dir
bench: build/tupu
	sh bench/bench.sh build/tupu $(PROG) $(FACTS)

clean:
	rm -rf build
.PHONY: all clang san test bench clean
