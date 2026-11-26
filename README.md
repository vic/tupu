# tupu

tupu is a Datalog engine written in C. You give it a Datalog program and some facts, and it tells you everything that follows from them. It reads the same kind of programs Soufflé reads (a subset, see below), so if you already have Soufflé rules you can usually run them as they are.

We built it because we needed a Datalog that is faster and lighter on memory than the engines we tried, and because we wanted answers we can trust byte for byte.

## What you get

- **The same answer every time.** The result does not depend on the order of your facts, the order of your rules, or how many cores tupu uses. One thread or all of them, the output is identical.
- **All your cores.** tupu spreads the work over every CPU by default; `-j` picks another number.
- **Numbers that never wrap.** Integers are exact up to 128 bits. If a rule would overflow or divide by zero, tupu stops and tells you, instead of handing you a wrong row.
- **It says no instead of guessing.** A missing facts file, a malformed row, an unsafe rule or a cycle through negation is refused with a clear code and the file and line that caused it. You never get a silently partial result.
- **A budget.** `-b` caps how many rows your rules may derive, so a runaway program fails fast instead of eating your memory.
- **It can explain itself.** With `-P`, tupu writes down, for every output row, the rule and the rows that produced it, and checks that each one really derives again.

## Using it

```console
$ tupu -F facts/ -D - program.dl
$ tupu -F facts/ -D out/ -j 8 -b 50000000 program.dl
$ mcpp -e utf8 -W0 program.dl | tupu -F facts/ -D - -
```

Facts are tab-separated files, one per `.input` relation, named `relation.facts`. Results go to stdout with `-D -`, or to `relation.csv` files in a directory. tupu has no preprocessor of its own: if your program uses `#include` or `#define`, run it through `mcpp` first, like the last line above.

## What it reads

Typed `.decl` relations with `symbol` and `number` columns, subtypes and unions, `.input` and `.output`, facts written in the program, and rules with negation, comparisons, arithmetic, `;` and the aggregates `count`, `sum`, `min` and `max`, and one subsumption form: `R(k.., a) <= R(k.., b) :- a <= b.` (or `b <= a`) makes R a lattice relation that keeps the best value of one number column per key; tupu admits a rule only when a missing dominated row cannot change it: the lattice variable is in no join, key or negation, every comparison stays true and every lattice head improves or stays when the lattice values improve (polarity over `+`, `-`, `max`, `min`), aggregates over a lattice relation are same-direction `max`/`min` of that column; `count`, `sum`, `=`, `!=` on a lattice value and `.output` of a lattice relation are refused. Components, `.init`, other subsumption forms, records, floats and string functions are not supported: tupu refuses a program that uses them rather than run part of it.

## C library

To run repeated checks in-process without re-parsing or reading files from disk, link `libtupu.a` and include `include/tupu.h`. One `TupuSession` keeps the parsed program, plans, tables, and worker thread pool warm across transactions: `tupu_reset` clears rows without freeing buffers, `tupu_add_row` inserts facts, `tupu_eval` runs semi-naive evaluation, and `tupu_rel_read_row` reads derived rows. On `tests/api_test.c`, 1,000 consecutive `tc` frames run in 7.57 ms (7.6 µs per frame) with 0 KB RSS growth.

## Benchmarks

tupu runs programs directly with no C++ or Rust compilation step. Even so, it beats compiled Soufflé and compiled FlowLog on wall time across our benchmark set (`tc`, `andersen`, `rsg` under `bench/`, compiled comparator times exclude their build time):

| benchmark | tupu (interpreter) | Soufflé (compiled) | FlowLog (compiled) |
|---|---|---|---|
| `tc -j 1` | 0.21 s / 8.5 MB | 0.95 s / 10.0 MB | 4.46 s / 326.8 MB |
| `tc -j 6` | 0.14 s / 8.9 MB | 0.97 s / 10.0 MB | 2.42 s / 675.5 MB |
| `andersen -j 1` | 0.99 s / 43.5 MB | 7.20 s / 24.7 MB | > 40 s (timeout) |
| `andersen -j 6` | 0.59 s / 49.1 MB | 7.27 s / 24.8 MB | > 15 s (timeout) |
| `rsg -j 1` | 0.09 s / 13.5 MB | 0.16 s / 10.1 MB | 0.37 s / 59.9 MB |
| `rsg -j 6` | 0.09 s / 14.6 MB | 0.17 s / 10.3 MB | 0.20 s / 147.8 MB |

Peak memory on `tc` is now lower than compiled Soufflé; `andersen` RSS dropped from 65 MB to 43.5 MB via dense 2D bitset deduplication. The tupu cells were measured again on 2026-11-26 at commit db58e6a4 (AMD Ryzen 5 7530U, `bench/row.sh`, quiet host); the comparator cells are from the earlier run.

## Status

Working and tested. Every change runs against Soufflé's test programs, the Stigi rule suite (607 cases, 0 diff), exact 64/128-bit arithmetic shapes, multi-thread determinism (`-j 1` equals `-j 2` equals `-j 6`), sanitizer builds, the C library 1,000-frame flat-RSS test, and provenance replay (`-P`).

```console
$ nix develop
$ just build
$ just lib
$ just test
```
