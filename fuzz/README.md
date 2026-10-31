<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->

# Fuzzing libomxdsp

libFuzzer harnesses over the entry points of libomxdsp that take data from outside: a control
port's word (any float, int or choice a host can put on it), a block's samples (finite, or any
float word at all), a rate, a table or a stream of MIDI messages. Each harness is a plain C
`LLVMFuzzerTestOneInput` and builds with `clang -fsanitize=fuzzer,address,undefined` and
`-DOMX_CONTRACTS`, so the library's own PRE and POST laws are read after every call as well:
a broken law, like a sanitizer report, is a crash.

None of this is part of `make test`.

| harness | what it feeds | the properties it holds the library to beyond "no crash" |
|---|---|---|
| `fuzz_fx` | every instance face under `include/omxdsp/fx/` (25 of them): init at a rate, resolve with raw ports, run a block | no contract broken; finite in gives finite out; an in-place run equals an out-of-place one; a refused rate leaves the face the identity |
| `fuzz_laws` | the scalar laws every port goes through: clamps, dB and linear, pan, balance and fader laws, ramp, wet/dry, the one-pole constructors | a clamp lands in its travel; constant-power pan; the balance law never boosts; a ramp stays between its ends; a mix stays between its inputs; a pole is in [0, 1) |
| `fuzz_eq` | a bank of up to 24 bands and the head filters, designed at a rate, then a block through the cascade and the bank's magnitude | every designed section finite and stable; at most as many sections as bands; a bank with nothing live is the identity and 0 dB; finite out |
| `fuzz_src` | a rate pair, the polyphase table, a take cut into blocks and flushed | the plan is the reduced ratio; a take is the same cut into blocks as whole, bit for bit; output bounded by the table's own gain; the identity plan returns the take |
| `fuzz_delay` | the fractional delay line (orders 3 and 5, any ring size, any delay word) and the integer pair with its cross-fade | a whole delay is the take delayed, bit for bit; a fractional one is bounded by the Lagrange weights; a fade stays between its two taps |
| `fuzz_note_map` | a stream of MIDI 1.0 channel messages through a leg's policy, with the policy changed in the middle | no note sticks and none is released that never sounded (a model of the downstream synth); `held_count` is the table's population; a note on is never delivered outside the leg's range |

There is no parser of presets, files or tables in libomxdsp: it holds no serialised format, and
its tables are compiled in. When one is added it gets a harness here.

## Running

    apt-get install -y clang      # clang with libFuzzer's runtime; no other dependency beyond omx-contract
    make fuzz                     # build every harness into build/fuzz/
    make fuzz-run FUZZ_TIME=60    # run each for 60 s from its seed corpus; red on any crash
    make fuzz-corpus              # regenerate fuzz/corpus/ (fuzz/gen-corpus.c)

`make fuzz` needs omx-contract like every other target (see `BUILDING.md`).

A crash leaves its input in `fuzz/findings/` (ignored by git) and the log beside it. To
reproduce one:

    build/fuzz/fuzz_fx fuzz/findings/fuzz_fx-crash-<sha1>

## The checks and their records

Each property is its own `if (...) abort();` in a harness, never `assert()` and never inside a
macro, and the comment above it says what it checks and carries a record, as OSS Verify's
`tools/check --checks` reads them (its `docs/build-contract.md`, "The checks of a fuzz target"):

```
/* A clamp lands inside its travel, whatever the word.
 * mutant: include/omxdsp/omx_param.h:37 `(v <= hi ? v : hi)` -> `v`
 * input: clamp */
```

A record is a one-line change of the library and the seed it shows on: with the change, that seed
makes the check abort; without it, the seed passes. A stop after a failed allocation of the
harness's own carries `not a property: <reason>` in place of a record. The line numbers are those
of the commit the OSS Verify enrollment pins, and `tools/check --checks` measures each record there.

Where a check needs an expected value the harness works it out itself: the gcd of a plan, the
stability of a section, the validity of a leg, whether a sample is finite, none of them through the
library's own function.

## The harnesses read their input as a grammar

The header comment of each `fuzz_*.c` is the grammar of its input. The seed corpus in
`fuzz/corpus/<harness>/` is shaped the way the harness reads it, written by `fuzz/gen-corpus.c`
from what the repository already holds (the instance faces' own port lists, the EQ's bands, the
converter's rate pairs, the note map's policies): nothing is fetched and no capture is read.
`fuzz/run.sh` never writes into the corpus; what the fuzzer finds goes to a scratch directory under
the build directory.

## What the harnesses leave alone

- The instance faces take the nine rates of the oracle grid. 32, 64 and 128 kHz are not declared
  rates, and at those every design records the PRE `rate-is-declared` by design
  (`test/fx/*_instance.test.c` say so); the harness skips that one token at those rates.
- A multiband stage whose corner moves between two blocks carries the old design's state into the
  new one, and the crossover's post `bands-partition-unity` is a law of one design: the harness
  skips that token, and the suite's own `xover` arm holds it where the design is fixed.
- A block of non-finite samples (the `fuzz_fx` hostile mode) breaks the PRE `finite-in` by the
  caller's own act; the ledger is not read then and the sanitizers are the oracle.
