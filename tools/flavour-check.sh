#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# flavour-check.sh — each archive carries its flavour, and a contracts consumer sees the compiled
# unit's contracts only through an archive built with them.
#
#  1. SYMBOLS. libomxdsp.a references neither the contract ledger nor the TSan runtime;
#     libomxdsp-contracts.a references the ledger and not TSan; libomxdsp-tsan.a references both.
#  2. CONTRACTS. test/omxdsp_flavour.c (built with the test flags, -DOMX_CONTRACTS) linked against
#     libomxdsp-contracts.a reads the oversampler's own precondition violation in its ledger.
#  3. BLIND CONTROL. The same program linked against libomxdsp.a must FAIL: a contracts consumer
#     on the plain archive is the mistake this check exists to catch.
#  4. TSAN. The same program linked against libomxdsp-tsan.a under -fsanitize=thread reads it too,
#     where the toolchain links and runs TSan programs; elsewhere NOT RUN (exit 0, never a pass).
#
# Usage: flavour-check.sh <scratch-dir> "<test cflags>" <plain.a> <contracts.a> <tsan.a>
set -uo pipefail
OUT="${1:?scratch dir}"; FLAGS="${2:?test cflags}"; PLAIN="${3:?}"; CONTRACTS="${4:?}"; TSAN="${5:?}"
CC="${CC:-cc}"; NM="${NM:-nm}"
mkdir -p "$OUT"
fail() { echo "flavour-check: FAIL — $*"; exit 1; }

refs() { "$NM" "$1" 2>/dev/null | grep -cE "$2"; }
for a in "$PLAIN" "$CONTRACTS" "$TSAN"; do [ -f "$a" ] || fail "$a is missing"; done
[ "$(refs "$PLAIN" ' omx_contract_log$')" -eq 0 ] || fail "$PLAIN references the contract ledger"
[ "$(refs "$PLAIN" '__tsan_')" -eq 0 ] || fail "$PLAIN is TSan-instrumented"
[ "$(refs "$CONTRACTS" ' omx_contract_log$')" -gt 0 ] || fail "$CONTRACTS has no contract compiled in"
[ "$(refs "$CONTRACTS" '__tsan_')" -eq 0 ] || fail "$CONTRACTS is TSan-instrumented"
[ "$(refs "$TSAN" ' omx_contract_log$')" -gt 0 ] || fail "$TSAN has no contract compiled in"
[ "$(refs "$TSAN" '__tsan_')" -gt 0 ] || fail "$TSAN is not TSan-instrumented"
echo "flavour-check: the three archives carry their flavours"

# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/contracts" test/omxdsp_flavour.c "$CONTRACTS" -lm || fail "the consumer does not link the contracts archive"
"$OUT/contracts" || fail "the contracts consumer on $CONTRACTS"

# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/plain" test/omxdsp_flavour.c "$PLAIN" -lm || fail "the consumer does not link the plain archive"
if out="$("$OUT/plain")"; then
  fail "the consumer linked against $PLAIN passed: the check cannot tell the flavours apart"
fi
printf '%s\n' "$out" | grep -q '^flavour: FAIL' || fail "the consumer on $PLAIN failed without naming the flavour: $out"
echo "flavour-check: on $PLAIN the same consumer fails, as it must"

printf 'int main(void) { return 0; }\n' > "$OUT/probe.c"
if $CC -fsanitize=thread -o "$OUT/probe" "$OUT/probe.c" 2>/dev/null && "$OUT/probe" 2>/dev/null; then
  # shellcheck disable=SC2086
  $CC $FLAGS -fsanitize=thread -o "$OUT/tsan" test/omxdsp_flavour.c "$TSAN" -lm || fail "the consumer does not link the TSan archive"
  TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$OUT/tsan" || fail "the TSan consumer on $TSAN"
else
  echo "flavour-check: NOT RUN — the TSan consumer: the toolchain does not link and run -fsanitize=thread here"
fi
echo "flavour-check: RAN — a contracts consumer reads the compiled unit's contracts through the contracts archive"
