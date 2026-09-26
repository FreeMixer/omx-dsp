#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# tsan-gate.sh — one N-thread test under ThreadSanitizer, with the sabotage that proves it can see
# (docs/design/specs/2026-09-26-dsp-primitives.md §4.3 (e)).
#
#  1. PROBE. An empty program compiled, linked AND run under -fsanitize=thread. Where any step
#     fails the toolchain has no TSan: print `tsan-gate: NOT RUN — <name>: <reason>` and exit 0 —
#     NOT RUN, never a pass. OMX_TSAN_REQUIRED=1 makes NOT RUN a failure. OMX_TSAN_LDFLAGS is added
#     to every link (a runtime outside the toolchain's default path).
#  2. CLEAN. The test built with -fsanitize=thread must exit 0 under halt_on_error=1 and print no
#     ThreadSanitizer report.
#  3. SABOTAGE. The same test built with -D<sabotage> (every worker handed ONE state) must print
#     `ThreadSanitizer: data race`. A race the gate does not see is a blind gate: FAIL.
#
# Usage: tsan-gate.sh <scratch-dir> <name> <sabotage-define> <cc sources and flags…>
set -uo pipefail
OUT="${1:?scratch dir}"; NAME="${2:?name}"; SABOTAGE="${3:?sabotage define}"; shift 3
CC="${CC:-cc}"
LDX="${OMX_TSAN_LDFLAGS:-}"
mkdir -p "$OUT"

not_run() {
  if [ "${OMX_TSAN_REQUIRED:-0}" = 1 ]; then echo "tsan-gate: FAIL — $NAME: TSan required and unavailable: $1"; exit 1; fi
  echo "tsan-gate: NOT RUN — $NAME: $1"; exit 0
}

printf 'int main(void) { return 0; }\n' > "$OUT/probe.c"
# shellcheck disable=SC2086
if ! err="$($CC -fsanitize=thread -o "$OUT/probe" "$OUT/probe.c" $LDX 2>&1)"; then
  not_run "the toolchain cannot link -fsanitize=thread ($(printf '%s\n' "$err" | grep -m1 -iE 'cannot|error' || printf '%s' "$err" | head -1))"
fi
if ! err="$("$OUT/probe" 2>&1)"; then
  not_run "a -fsanitize=thread program does not run here ($(printf '%s\n' "$err" | head -1))"
fi

export TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}"
# shellcheck disable=SC2086
$CC -fsanitize=thread -g "$@" $LDX -o "$OUT/$NAME" || { echo "tsan-gate: FAIL — $NAME does not build under TSan"; exit 1; }
out="$("$OUT/$NAME" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] || printf '%s\n' "$out" | grep -q 'ThreadSanitizer'; then
  printf '%s\n' "$out" | tail -40
  echo "tsan-gate: FAIL — $NAME under ThreadSanitizer (exit $rc)"; exit 1
fi

# shellcheck disable=SC2086
$CC -fsanitize=thread -g "-D$SABOTAGE" "$@" $LDX -o "$OUT/$NAME.sabotage" || { echo "tsan-gate: FAIL — the $NAME sabotage does not build"; exit 1; }
sab="$("$OUT/$NAME.sabotage" 2>&1)"; src=$?
if ! printf '%s\n' "$sab" | grep -q 'ThreadSanitizer: data race' || [ "$src" -eq 0 ]; then
  printf '%s\n' "$sab" | tail -20
  echo "tsan-gate: FAIL — $NAME: the shared-state sabotage ($SABOTAGE) raced unseen (exit $src): the gate is blind"; exit 1
fi
echo "tsan-gate: RAN — $NAME clean under ThreadSanitizer; the shared-state sabotage raced and was caught (exit $src)"
