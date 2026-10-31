#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gen-once-loud.sh <scratch-dir> [gen-once.sh] — when the scratch build inside gen-once.sh fails,
# the script must say so on stderr and exit non-zero (FreeMixer/omx-dsp#206).
#
# count_runs runs inside $(…), which captures stdout. A failure text written to stdout lands in the
# caller's variable and set -e ends the script with no message; on debian:trixie that left
# `make: *** [gen-once] Error 1` and nothing else. Here `nice`, which starts every scratch build, is
# replaced by a shim that prints a marker and fails; gen-once.sh is run and its stderr must hold the
# "build failed" line and the marker. The optional second argument names the script to run, so a
# copy without the fix can be shown to fail this test.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?scratch dir}"
SUT="${2:-$HERE/gen-once.sh}"
rm -rf "$OUT" && mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho loud-marker-build-broke\nexit 2\n' > "$OUT/bin/nice"
chmod +x "$OUT/bin/nice"

set +e
PATH="$OUT/bin:$PATH" bash "$SUT" "$OUT/run" > "$OUT/stdout" 2> "$OUT/stderr"
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "gen-once-loud.sh: FAIL — a failing build exited 0"; exit 1; }
grep -q 'build failed' "$OUT/stderr" || { echo "gen-once-loud.sh: FAIL — no 'build failed' line on stderr (exit $rc)"; exit 1; }
grep -q 'loud-marker-build-broke' "$OUT/stderr" || { echo "gen-once-loud.sh: FAIL — the build output's tail is not on stderr"; exit 1; }
echo "gen-once-loud.sh: OK — a failing scratch build is reported on stderr"
