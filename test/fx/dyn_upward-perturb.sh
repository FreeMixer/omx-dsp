#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# dyn_upward-perturb.sh — test/fx/dyn_upward.test.c sees each defect of the upward law. Each sabotage
# is applied to a COPY of the include directory placed ahead of the real one, and the oracle built
# against it must go red in the named arm:
#   clip      — the 4x path's clamp back at the make-up, which cuts the boost: arm B;
#   slope     — the upward slope read as the expander's, R - 1: arm A;
#   cap       — the range not capping the boost: arm A.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… dyn_upward-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# The sabotages are independent and run side by side; each is waited for and every one must be seen.
pids=()

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>...
  local name="$1" hdr="$2" expr="$3" d="$OUT/$1"
  shift 3
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "dyn_upward-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/dyn_upward.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "dyn_upward-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "dyn_upward-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "dyn_upward-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage clip omx_dyn.h 's|const float ceil_lin = p->gc.mode == OMX_DYN_UPWARD ? omx_db_to_lin(p->gc.range_db) \* p->gc.makeup_lin : p->gc.makeup_lin;|const float ceil_lin = p->gc.makeup_lin;|' B &
pids+=($!)
sabotage slope omx_gaincomp.h 's|const float ku = slope \* eu \* eu|const float ku = (1.0f - p->ratio) * eu * eu|' A &
pids+=($!)
sabotage cap omx_gaincomp.h 's|gu = gu > p->range_db ? p->range_db : gu;|gu = gu;|' A &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "dyn_upward-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "dyn_upward-perturb: the upward oracle sees all three sabotages"
