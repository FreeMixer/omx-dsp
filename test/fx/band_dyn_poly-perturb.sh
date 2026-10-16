#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# band_dyn_poly-perturb.sh — test/fx/band_dyn_poly.test.c sees each defect of the libm-free
# band-dynamics loop. Each sabotage is applied to a COPY of omx_band_dyn.h placed ahead of the real
# include directory, and the oracle built against it must go red in the named arm:
#   level  — the level is read 0.1 % hot in dB before the characteristic: the gain and output arms;
#   unity  — the gain is 1e-5 relative low, inside the output bound but never exactly 1: the unity arm;
#   chunk  — the chunk applies its LAST sample's gain to every sample: the chunk arm;
#   state  — the detector folds the legs 0.51/0.51: the state arm.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… band_dyn_poly-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # name sed-expression arm...
  local name=$1 expr=$2 d="$OUT/$1"
  shift 2
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_band_dyn.h"
  if cmp -s "$PKG/include/omxdsp/omx_band_dyn.h" "$d/omxdsp/omx_band_dyn.h"; then
    echo "band_dyn_poly-perturb: FAIL — sabotage '$name' matched nothing in omx_band_dyn.h"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS \
    -o "$d/oracle" "$PKG/test/fx/band_dyn_poly.test.c" -lm
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "band_dyn_poly-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm
  for arm in "$@"; do
    grep -q "^FAIL \[$arm\]" "$d/oracle.log" \
      || { echo "band_dyn_poly-perturb: FAIL — sabotage '$name' did not turn the $arm arm red"; head -5 "$d/oracle.log"; exit 1; }
  done
  echo "band_dyn_poly-perturb: sabotage '$name' turned the $* arm(s) red"
}

sabotage level 's|omx_gaincomp_db(&c, omx_lin_to_db_poly(level));|omx_gaincomp_db(\&c, 1.001f * omx_lin_to_db_poly(level));|' gain output
sabotage unity 's|  const float g = omx_db_to_lin_poly(omx_band_dyn_offset_db(gc, level));|  const float g = 0.99999f * omx_db_to_lin_poly(omx_band_dyn_offset_db(gc, level));|' unity
sabotage chunk 's|g\[i\] = omx_band_dyn_gain(&dyn->gc, lv\[i\]);|g[i] = omx_band_dyn_gain(\&dyn->gc, lv[m - 1u]);|' chunk
sabotage state 's|  const float k = stereo ? 0.5f \* (fabsf(\*bl) + fabsf(\*br)) : fabsf(\*bl);|  const float k = stereo ? 0.51f * (fabsf(*bl) + fabsf(*br)) : fabsf(*bl);|' state
echo "band_dyn_poly-perturb: the band-dynamics poly oracle sees all four sabotages"
