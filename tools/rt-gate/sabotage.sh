#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sabotage.sh — omx-rt-gate's guards broken one at a time; each must turn its self-test red.
#
#   tools/rt-gate/sabotage.sh GATE TABLES ARM ["FLAGS"]
#
#   1. the forbidden list emptied (a literal empty file, refused at load; and a list holding one
#      inert row, which loads): the planted fixtures must stop reading their reds, so selftest.sh
#      must FAIL on them;
#   2. the class table emptied: the clean twins must go red on their maths and memory calls, so
#      selftest.sh must FAIL on them;
#   3. -O2 is no input: the planted malloc of fixtures/alloc-run.c is in the -O0 -fno-inline call
#      graph and gone from the -O2 one (dead-code removal), which is why the gate compiles at -O0;
#   4. the externals arm's own check: a case that allocates must read red (ARM --sabotage);
#   5. the self-run (with FLAGS, the library's include and build flags): a header under a scratch
#      include/omxdsp with a function tagged `@thread audio` that allocates must read RT-ALLOC through
#      the generated unit, and one without its tag must stop the tag check (TAG-MISSING).
# The self-test runs green over the shipped tables first, or nothing is judged (exit 2).
# Exit 0 when every sabotage was caught, 1 otherwise.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GATE="$1"
TABLES="$2"
ARM="$3"
FLAGS="${4:-}"
CC="${CC:-cc}"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/omx-rt-gate-sabotage.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
fail=0

caught() { # caught <label> <log> <pattern the FAIL lines must match>
  if grep -q '^selftest: RED' "$2" && grep '^selftest: FAIL' "$2" | grep -q -- "$3"; then
    echo "sabotage: ok   $1: $(grep -c '^selftest: FAIL' "$2") self-test cases red"
  else
    echo "sabotage: FAIL $1 left the self-test green: the guard is decoration"
    grep '^selftest: ' "$2" | tail -5 | sed 's/^/sabotage:   /'
    fail=1
  fi
}

# the baseline first: a sabotage only means something against a self-test that is green unbroken
if ! bash "$HERE/selftest.sh" "$GATE" "$TABLES" > "$SCRATCH/0.log" 2>&1; then
  grep '^selftest: FAIL' "$SCRATCH/0.log" | head -5 | sed 's/^/sabotage:   /'
  echo "sabotage: NOT-JUDGED the self-test is red before any sabotage"
  exit 2
fi
echo "sabotage: ok   the self-test is green over the shipped tables"

# 1a. the forbidden list, literally empty
cp -r "$TABLES" "$SCRATCH/t1a"
: > "$SCRATCH/t1a/forbidden.tbl"
bash "$HERE/selftest.sh" "$GATE" "$SCRATCH/t1a" > "$SCRATCH/1a.log" 2>&1
caught "the forbidden list empty" "$SCRATCH/1a.log" "fixtures/alloc-run.c"
# 1b. the forbidden list emptied of every row but an inert one: it loads, and nothing is forbidden
cp -r "$TABLES" "$SCRATCH/t1b"
echo "sabotage_never_called RT-IO the one row left by the sabotage" > "$SCRATCH/t1b/forbidden.tbl"
bash "$HERE/selftest.sh" "$GATE" "$SCRATCH/t1b" > "$SCRATCH/1b.log" 2>&1
caught "the forbidden list emptied to one inert row" "$SCRATCH/1b.log" "fixtures/lock.c"

# 2. the class table emptied
cp -r "$TABLES" "$SCRATCH/t2"
grep '^#' "$TABLES/classes.tbl" > "$SCRATCH/t2/classes.tbl"
bash "$HERE/selftest.sh" "$GATE" "$SCRATCH/t2" > "$SCRATCH/2.log" 2>&1
caught "the class table empty" "$SCRATCH/2.log" "\.clean\.c"

# 3. -O2 hides the planted malloc; -O0 -fno-inline shows it
(cd "$HERE" && "$CC" -Ifixtures -O0 -fno-inline -fcallgraph-info -c fixtures/alloc-run.c -o "$SCRATCH/o0.o") &&
(cd "$HERE" && "$CC" -Ifixtures -O2 -fcallgraph-info -c fixtures/alloc-run.c -o "$SCRATCH/o2.o")
if grep -q 'targetname: "malloc"' "$SCRATCH/o0.ci" && ! grep -q 'targetname: "malloc"' "$SCRATCH/o2.ci"; then
  echo "sabotage: ok   -O2 is no input: the planted malloc is in the -O0 graph and gone from the -O2 one"
else
  echo "sabotage: FAIL the -O0/-O2 arm: malloc in -O0: $(grep -c 'targetname: "malloc"' "$SCRATCH/o0.ci"), in -O2: $(grep -c 'targetname: "malloc"' "$SCRATCH/o2.ci")"
  fail=1
fi

# 4. the externals arm reads an allocation
if "$ARM" --sabotage > "$SCRATCH/4.log" 2>&1; then
  echo "sabotage: ok   $(tail -1 "$SCRATCH/4.log")"
else
  echo "sabotage: FAIL $(tail -1 "$SCRATCH/4.log")"
  fail=1
fi

# 5. the self-run sees a planted allocation in a tagged function, and the tag check an untagged one
if [ -n "$FLAGS" ]; then
  P="$SCRATCH/planted"
  mkdir -p "$P/include/omxdsp"
  cat > "$P/include/omxdsp/omx_planted.h" <<'EOF'
#include <stdlib.h>
/** @brief A planted allocation behind an audio tag. @thread audio */
static inline void omx_planted_grow(unsigned n) { void *p = malloc(n); free(p); }
/** @brief A planted function with no tag. */
static inline void omx_planted_untagged(void) {}
EOF
  echo '#include <omxdsp/omx_planted.h>' > "$P/headers.c"
  # shellcheck disable=SC2086 # FLAGS is a flag list
  (cd "$P" && "$GATE" --thread-tags "$P/planted.tags" --selfrun "$P/selfrun.c" --under include/omxdsp --work "$P/w1" \
    -- "$CC" -Iinclude $FLAGS headers.c) > "$P/tags.out" 2>&1
  rc=$?
  if [ "$rc" = 1 ] && grep -q '^TAG-MISSING omx_planted_untagged ' "$P/tags.out"; then
    echo "sabotage: ok   the tag check stops on a function without its tag"
  else
    echo "sabotage: FAIL the tag check let an untagged function through (exit $rc)"; fail=1
  fi
  # shellcheck disable=SC2086
  (cd "$P" && "$GATE" --tables "$TABLES" --abi omxdsp --tags "$P/planted.tags" --work "$P/w2" \
    -- "$CC" -Iinclude $FLAGS selfrun.c) > "$P/run.out" 2>&1
  rc=$?
  if [ "$rc" = 1 ] && grep -q '^RT-ALLOC malloc at include/omxdsp/omx_planted.h:3:.* via omx_planted_grow\[audio\]$' "$P/run.out"; then
    echo "sabotage: ok   the self-run reads the planted allocation in a tagged audio function"
  else
    echo "sabotage: FAIL the self-run did not read the planted allocation (exit $rc)"
    grep -v '^SUMMARY' "$P/run.out" | head -5 | sed 's/^/sabotage:   /'
    fail=1
  fi
fi

if [ "$fail" = 0 ]; then echo "sabotage: green, every guard caught"; else echo "sabotage: RED"; fi
exit "$fail"
