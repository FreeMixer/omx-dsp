#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# doppler-sabotage.sh <dir> — the Doppler head's suite arm (test/kernels/doppler.c) sees each
# defect of its fade and its hand-over. Each sabotage is applied to a COPY of omx_doppler.h placed
# ahead of the real include directory, the suite is built against it and must go red on its doppler
# arm:
#   curve    — the head's crossfade weight squared, not linear: the fade law;
#   fast     — the hand-over's weight falls twice as fast: the hand-over law and its length;
#   held     — the hand-over fades from the new head, not from the held read: the hand-over law.
# The tree is never edited. A sed that matches nothing is a failure, not a skip, and a red that
# printed no FAIL line of the doppler arm (a compile error, a crash) is not a red arm.
# Usage: CC=… CFLAGS=… doppler-sabotage.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# the arm list into the scratch dir, never build/: a parallel make builds the suite from build/'s
bash "$PKG/tools/kernels-gen.sh" "$OUT" >/dev/null

sabotage() { # name sed-expression
  local name=$1 expr=$2 d="$OUT/$1" rc=0
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_doppler.h"
  if cmp -s "$PKG/include/omxdsp/omx_doppler.h" "$d/omxdsp/omx_doppler.h"; then
    echo "doppler-sabotage.sh: FAIL — sabotage '$name' changed nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS \
    -I"$OUT" -o "$d/suite" "$PKG/test/omxdsp_suite.c" "$PKG"/src/*.c -lm
  "$d/suite" > "$d/out" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "doppler-sabotage.sh: FAIL — sabotage '$name' left the suite GREEN; the arm cannot see the defect"
    return 1
  fi
  local n
  n=$(grep -c '^FAIL \[doppler[] ]' "$d/out" || true)
  if [ "$n" -eq 0 ]; then
    echo "doppler-sabotage.sh: FAIL — sabotage '$name' went red (rc=$rc) with no FAIL line of the doppler arm"
    tail -5 "$d/out"
    return 1
  fi
  echo "doppler-sabotage.sh: sabotage '$name': the doppler arm went red on $n checks"
}

sabotage curve 's|    const float g = (float)(q / ((double)d->fade \* w));|    const float g = (float)((q / ((double)d->fade * w)) * (q / ((double)d->fade * w)));|'
sabotage fast 's|    st->hand = st->hand > st->hand_step ? st->hand - st->hand_step : 0.0f;|    st->hand = st->hand > 2.0f * st->hand_step ? st->hand - 2.0f * st->hand_step : 0.0f;|'
sabotage held 's|    float yh = omx_fdelay_read(line, st->held\[0\]);|    float yh = omx_fdelay_read(line, d0);|'
echo "doppler-sabotage.sh: the doppler arm sees all three sabotages"
