# tupu build. Run inside `nix-shell` (shell.nix). Every command is run under timeout by the callers.
CC ?= gcc
WARN = -std=c11 -Wall -Wextra -Werror -pedantic
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

all: build/tupu
san: build/tupu-san

build/tupu: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(WARN) -O2 -DNDEBUG -o $@ $(SRC)

build/tupu-san: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(WARN) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ $(SRC)

# the oracle runs the release build and the sanitizer build
test: build/tupu build/tupu-san
	BIN=build/tupu sh tests/run.sh
	BIN=build/tupu-san sh tests/run.sh

# bench PROGRAM FACTS_DIR: wall, instructions, peak RSS, page faults; 3 runs, median
bench: build/tupu
	sh bench/bench.sh build/tupu $(PROG) $(FACTS)

clean:
	rm -rf build
.PHONY: all san test bench clean
