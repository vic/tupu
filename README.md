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

Typed `.decl` relations with `symbol` and `number` columns, subtypes and unions, `.input` and `.output`, facts written in the program, and rules with negation, comparisons, arithmetic, `;` and the aggregates `count`, `sum`, `min` and `max`. Components, `.init`, subsumption, records, floats and string functions are not supported: tupu refuses a program that uses them rather than run part of it.

## Status

Early: the engine is being written now. Every change is checked against Soufflé, on Soufflé's own tests and on the rule suite of the Stigi project, and measured against Soufflé and FlowLog for time and memory. The goal is to beat both; the numbers will be here once we have them.

```console
$ nix develop
$ just build
$ just test
```
