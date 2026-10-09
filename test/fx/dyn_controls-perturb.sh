#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# dyn_controls-perturb.sh — each dynamics control's oracle (test/fx/dyn_controls.test.c) goes red
# when its feature is broken. Four SABOTAGED copies of the include directory, one feature each:
#   hold        the hold is never re-armed while the level is above the close point
#   hysteresis  the open curve's close point is the threshold itself
#   knee        the gain computer's soft knee is off
#   mix         the dry share is never blended in
# Each is built against the oracle and must FAIL in its own arm ("FAIL [<arm>]"). The tree is never
# edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… dyn_controls-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # <arm> <header> <sed expression>
  local arm="$1" hdr="$2" expr="$3" dir="$OUT/$1"
  mkdir -p "$dir/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$dir/omxdsp/"
  sed -i "$expr" "$dir/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$dir/omxdsp/$hdr"; then
    echo "dyn_controls-perturb: FAIL — the $arm sabotage matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$dir/oracle" "$PKG/test/fx/dyn_controls.test.c" "$PKG"/src/*.c -lm
  if "$dir/oracle" >"$dir/oracle.log"; then
    echo "dyn_controls-perturb: FAIL — the $arm sabotage left the oracle green"; exit 1
  fi
  grep -q "^FAIL \[$arm\]" "$dir/oracle.log" || {
    echo "dyn_controls-perturb: FAIL — the $arm sabotage went red, but not in the $arm arm"; head -5 "$dir/oracle.log"; exit 1; }
  echo "sabotaged $arm: $(grep -c "^FAIL \[$arm\]" "$dir/oracle.log") $arm checks red"
}

sabotage hold omx_dyn.h 's|if (vdb >= open_gc.thresh_db) st->hold_left = hold;|if (vdb >= open_gc.thresh_db) st->hold_left = 0u;|'
sabotage hysteresis omx_dyn.h 's|open_gc.thresh_db = p->gc.thresh_db - p->hyst_db;|open_gc.thresh_db = p->gc.thresh_db;|'
sabotage knee omx_gaincomp.h 's|const int soft = p->knee_db > 0.0f;|const int soft = 0;|'
sabotage mix omx_dyn.h 's|if (p->dry > 0.0f) omx_dyn_mix_gain(gain, m, p->dry);|if (p->dry > 2.0f) omx_dyn_mix_gain(gain, m, p->dry);|'
echo "dyn_controls-perturb: each control's oracle went red when its feature was broken"
