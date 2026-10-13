#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# dyn_flat-perturb.sh — test/fx/dyn_flat.test.c sees each defect of the flat-gain path (#56). Each
# sabotage is applied to a COPY of omx_dyn.h placed ahead of the real include directory, and the
# oracle built against it must go red in the named arm:
#   off    — the kernel runs the gain computer on every sample, as before #56: the calls arm;
#   open   — the open bound is taken two steps below where its branch was seen to hold: the
#            selector arm;
#   floor  — the floor constant is the gain computer's at twice the floor bound, inside the knee:
#            the selector and kernel arms.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… dyn_flat-perturb.sh <scratch-dir>
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
  sed -i "$expr" "$d/omxdsp/omx_dyn.h"
  if cmp -s "$PKG/include/omxdsp/omx_dyn.h" "$d/omxdsp/omx_dyn.h"; then
    echo "dyn_flat-perturb: FAIL — sabotage '$name' matched nothing in omx_dyn.h"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS \
    -o "$d/oracle" "$PKG/test/fx/dyn_flat.test.c" "$PKG"/src/*.c -lm
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "dyn_flat-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm
  for arm in "$@"; do
    grep -q "^FAIL \[$arm\]" "$d/oracle.log" \
      || { echo "dyn_flat-perturb: FAIL — sabotage '$name' did not turn the $arm arm red"; head -5 "$d/oracle.log"; exit 1; }
  done
  echo "dyn_flat-perturb: sabotage '$name' turned the $* arm(s) red"
}

sabotage off 's|  const int flat = p->gc.mode == OMX_DYN_BELOW \&\& !gate_state;|  const int flat = 0;|' calls
sabotage open 's|    f->hi = h \* step;|    f->hi = h / step / step;|' selector
sabotage floor 's|    f->g_floor = omx_gaincomp_gain_shifted(\&p->gc, p->knee_shift_db, f->lo);|    f->g_floor = omx_gaincomp_gain_shifted(\&p->gc, p->knee_shift_db, 2.0f * f->lo);|' selector kernel
echo "dyn_flat-perturb: the flat-gain oracle sees all three sabotages"
