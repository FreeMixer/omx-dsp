#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# align-perturb.sh — the alignment stage's tests see a broken slew. Two SABOTAGED copies of
# omx_align.h: one moves the read point up by twice OMX_ALIGN_SLEW per sample, the other declares
# the slew 1/31 instead of 1/32 (one step off, #75). Built against each,
# test/fx/align_fdelay.test.c must go red in its landing arm (a move no longer lands after the
# declared number of samples) and test/fx/align_golden.test.c must move at every rate it holds.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… align-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # name sed-expression
  local name=$1 expr=$2 d="$OUT/$1"
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_align.h"
  if cmp -s "$PKG/include/omxdsp/omx_align.h" "$d/omxdsp/omx_align.h"; then
    echo "align-perturb: FAIL — the $name sabotage matched nothing in omx_align.h"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/align_fdelay.test.c" "$PKG"/src/*.c -lm
  # shellcheck disable=SC2086
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -include "$PKG/test/support/log10f_subst.h" \
    -o "$d/golden" "$PKG/test/fx/align_golden.test.c" "$PKG"/src/*.c -lm
  if "$d/oracle" >"$d/oracle.log"; then echo "align-perturb: FAIL — the $name sabotage left align_fdelay green"; exit 1; fi
  grep -q '^FAIL a 10 -> 40 move lands exactly' "$d/oracle.log" \
    || { echo "align-perturb: FAIL — the $name sabotage did not turn the landing arm red"; exit 1; }
  local red g
  red="$(grep -c '^FAIL' "$d/oracle.log" || true)"
  "$d/golden" "$PKG/test/golden/align.sha256" >"$d/golden.log" 2>&1 || true
  g="$(grep '^fx/align_golden:' "$d/golden.log")"
  [[ "$g" =~ ([0-9]+)\ rates,\ ([0-9]+)\ moved ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ] \
    || { echo "align-perturb: FAIL — the $name sabotage did not move every digest: $g"; exit 1; }
  echo "align-perturb: the $name sabotage turned $red align_fdelay checks red and moved the digest at ${BASH_REMATCH[1]} rates"
}

sabotage double-slew 's|cur = (gap > OMX_ALIGN_SLEW) ? cur + OMX_ALIGN_SLEW|cur = (gap > OMX_ALIGN_SLEW) ? cur + 2.0f * OMX_ALIGN_SLEW|'
sabotage slew-1/31 's|#define OMX_ALIGN_SLEW (1.0f / 32.0f)|#define OMX_ALIGN_SLEW (1.0f / 31.0f)|'
