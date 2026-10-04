#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# tsan-gate.sh — one N-thread test under ThreadSanitizer, with the sabotage that proves it can see
# (docs/design/specs/2026-09-26-dsp-primitives.md §4.3 (e)).
#
#  1. PROBE. An empty program compiled, linked AND run under -fsanitize=thread. Where any step
#     fails the toolchain has no TSan: print `UNJUDGED: tsan-gate <name> — <reason>` and exit 0 —
#     never a pass; the pre-settle gate refuses the line naming the image (rt-thread-split §5.1).
#     OMX_TSAN_REQUIRED=1 makes it a failure. OMX_TSAN_LDFLAGS is added
#     to every link (a runtime outside the toolchain's default path).
#  2. CLEAN. The test built with -fsanitize=thread must exit 0 under halt_on_error=1 and print no
#     ThreadSanitizer report. The run's whole output is kept in <scratch>/<name>.log; a red prints
#     every report WHOLE (header, both stacks, summary) and the test's last lines
#     (tools/tsan-gate-selftest.sh holds it to that).
#     A standalone atomic_thread_fence is not modelled by TSan (gcc -Wtsan): the build keeps it a
#     warning and the gate prints each site as `tsan-gate: NOTE — … fence unmodelled`, so a
#     publication TSan cannot check is named, never silently trusted (§4.2 F10).
#     Before the FAIL line the gate prints the report count and the FIRST report's block as
#     `tsan-gate: report:` lines (header, both stacks, summary; at most 40 lines of 200 characters),
#     so a pod shard's log carries the racing pair after the scratch log is gone.
#  3. SABOTAGE. The same test built with -D<sabotage> (every worker handed ONE state) must print
#     `ThreadSanitizer: data race`. A race the gate does not see is a blind gate: FAIL.
#
# Usage: tsan-gate.sh <scratch-dir> <name> <sabotage-define> <cc sources and flags…>
set -uo pipefail
OUT="${1:?scratch dir}"; NAME="${2:?name}"; SABOTAGE="${3:?sabotage define}"; shift 3
CC="${CC:-cc}"
LDX="${OMX_TSAN_LDFLAGS:-}"
mkdir -p "$OUT"

# report_block <log> — the report count and the first report's block, bounded, prefixed.
report_block() {
  local n
  n="$(grep -c '^WARNING: ThreadSanitizer' "$1")"
  [ "$n" -gt 0 ] || return 0
  echo "tsan-gate: report: $n ThreadSanitizer report(s) in $1"
  awk '/^WARNING: ThreadSanitizer/ { on = 1 }
       on { if (++k <= 40) print "tsan-gate: report: " substr($0, 1, 200) }
       on && /^SUMMARY: ThreadSanitizer/ { exit }' "$1"
}

not_run() {
  if [ "${OMX_TSAN_REQUIRED:-0}" = 1 ]; then echo "tsan-gate: FAIL — $NAME: TSan required and unavailable: $1"; exit 1; fi
  echo "UNJUDGED: tsan-gate $NAME — $1"; exit 0
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
if ! blog="$($CC -fsanitize=thread -Wno-error=tsan -g "$@" $LDX -o "$OUT/$NAME" 2>&1)"; then
  printf '%s\n' "$blog" | tail -20; echo "tsan-gate: FAIL — $NAME does not build under TSan"; exit 1
fi
printf '%s\n' "$blog" | grep -E "atomic_thread_fence' is not supported" | sed -E 's/^([^:]+:[0-9]+):.*/tsan-gate: NOTE — \1: atomic_thread_fence, a publication TSan does not model/' | sort -u
"$OUT/$NAME" > "$OUT/$NAME.log" 2>&1; rc=$?
if [ "$rc" -ne 0 ] || grep -q 'ThreadSanitizer' "$OUT/$NAME.log"; then
  # the test's last lines before any report, then every report WHOLE — header, both stacks, the
  # location, the threads, the summary; a tail alone cuts a deep report's header and racing access
  awk '/^WARNING: ThreadSanitizer/ { exit } { print }' "$OUT/$NAME.log" | tail -20
  awk '/^WARNING: ThreadSanitizer/ { on = 1 } on { print } /^SUMMARY: ThreadSanitizer/ { on = 0 }' "$OUT/$NAME.log"
  report_block "$OUT/$NAME.log"
  echo "tsan-gate: FAIL — $NAME under ThreadSanitizer (exit $rc); the whole run: $OUT/$NAME.log"; exit 1
fi

# shellcheck disable=SC2086
$CC -fsanitize=thread -Wno-error=tsan -Wno-tsan -g "-D$SABOTAGE" "$@" $LDX -o "$OUT/$NAME.sabotage" || { echo "tsan-gate: FAIL — the $NAME sabotage does not build"; exit 1; }
"$OUT/$NAME.sabotage" > "$OUT/$NAME.sabotage.log" 2>&1; src=$?
sab="$(cat "$OUT/$NAME.sabotage.log")"
if ! printf '%s\n' "$sab" | grep -q 'ThreadSanitizer: data race' || [ "$src" -eq 0 ]; then
  printf '%s\n' "$sab" | tail -20
  report_block "$OUT/$NAME.sabotage.log"
  echo "tsan-gate: FAIL — $NAME: the shared-state sabotage ($SABOTAGE) raced unseen (exit $src): the gate is blind"; exit 1
fi
report_block "$OUT/$NAME.sabotage.log"
echo "tsan-gate: RAN — $NAME clean under ThreadSanitizer; the shared-state sabotage raced and was caught (exit $src)"
